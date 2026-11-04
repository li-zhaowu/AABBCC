// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "vncVideoStreamer.h"
#include "vncVideoCapture.h"
#include "vncNvencEncoder.h"
#include "common/RateController.h"

#include <dxgi.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

using namespace video;
using extmsg::kPrioControl;
using extmsg::kPrioVideo;
using Clock = std::chrono::steady_clock;

namespace {

uint64_t NowUs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		Clock::now().time_since_epoch()).count());
}

bool SameRect(const RECT& a, const RECT& b)
{
	return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// While Windows switches desktops (UAC prompt, lock screen) or changes the
// display mode, duplication is unavailable for a moment.
constexpr auto kRecoverTimeout = std::chrono::seconds(5);
constexpr auto kAreaCheckInterval = std::chrono::seconds(1);
constexpr auto kStatsInterval = std::chrono::seconds(10);

// Bitrate adaptation: common/RateController.
constexpr uint32_t kMinKbps = RateController::kMinKbps;

// Open the encoder with the best codec that the viewer decodes and this GPU
// encodes. A codec that fails is dropped from `candidates`, so that later
// re-opens (resolution changes) do not try it again.
bool OpenBestCodec(vncNvencEncoder& encoder, ID3D11Device* device, int width, int height, int fps,
                   uint32_t kbps, std::vector<uint8_t>& candidates, uint8_t& codec, std::string& error,
                   const std::function<void(const std::string&)>& log)
{
	while (!candidates.empty()) {
		codec = candidates.front();
		if (encoder.Open(device, width, height, fps, kbps, codec, error))
			return true;
		if (candidates.size() == 1)
			return false; // keep the last one: the error may be temporary
		const uint8_t next = candidates[1];
		log(std::string(CodecName(codec)) + " not available (" + error + "), trying " + CodecName(next));
		candidates.erase(candidates.begin());
	}
	error = "no codec to encode with";
	return false;
}

// D3D11 device on the first NVIDIA adapter (for the external stream, which
// has no capture device of its own).
bool CreateNvidiaDevice(ID3D11Device** device, ID3D11DeviceContext** context, std::string& error)
{
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
		error = "CreateDXGIFactory1 failed";
		return false;
	}
	IDXGIAdapter1* adapter = nullptr;
	bool ok = false;
	for (UINT i = 0; !ok && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE)
			ok = SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
				NULL, 0, D3D11_SDK_VERSION, device, NULL, context));
		adapter->Release();
	}
	factory->Release();
	if (!ok)
		error = "no usable NVIDIA GPU for the window video";
	return ok;
}

// Skip encoding while more than about three frames wait to be sent.
size_t QueueBudget(uint32_t kbps, int fps)
{
	return (std::max)(size_t(256 * 1024), size_t(kbps) * 1000 / 8 / fps * 3);
}

} // namespace

vncVideoStreamer::vncVideoStreamer(Callbacks cb) : m_cb(std::move(cb)) {}

vncVideoStreamer::~vncVideoStreamer()
{
	Shutdown();
}

bool vncVideoStreamer::IsSupported(std::string* why)
{
	static std::once_flag once;
	static bool supported = false;
	static std::string reason;
	std::call_once(once, [] {
		if (!vncNvencEncoder::IsAvailable(&reason))
			return;
		if (!vncVideoCapture::HasNvidiaAdapter()) {
			reason = "no NVIDIA GPU found";
			return;
		}
		supported = true;
	});
	if (!supported && why)
		*why = reason;
	return supported;
}

void vncVideoStreamer::Log(int level, const std::string& msg)
{
	if (m_cb.log)
		m_cb.log(level, "video: " + msg);
}

void vncVideoStreamer::SendStatus(uint32_t code, const std::string& message)
{
	Status st;
	st.code = code;
	st.message = message;
	m_cb.send(kPrioControl, kStatus, EncodeStatus(st));
	if (code != kStatusStopped)
		Log(0, "stopped: " + message);
}

void vncVideoStreamer::OnMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	switch (subtype) {
	case kStart: {
		Start start;
		if (!DecodeStart(payload.data(), payload.size(), start)) {
			SendStatus(kStatusBadRequest, "malformed start request");
			return;
		}
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_lastStart = start;
			m_haveStart = true;
		}
		if (start.flags & kStartParamsOnly)
			return; // a window session will use them (BeginExternal)
		std::lock_guard<std::mutex> control(m_controlMutex);
		StopThread();
		m_external = false;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_stop = false;
			m_wantKeyframe = false;
		}
		m_thread = std::thread(&vncVideoStreamer::ThreadMain, this, start);
		break;
	}
	case kStop: {
		std::lock_guard<std::mutex> control(m_controlMutex);
		StopThread();
		m_external = false;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_haveStart = false; // video mode is off: no window video either
		}
		SendStatus(kStatusStopped, "");
		break;
	}
	case kFeedback: {
		Feedback fb;
		if (DecodeFeedback(payload.data(), payload.size(), fb) && (fb.flags & kFeedbackKeyframe)) {
			std::lock_guard<std::mutex> lk(m_mutex);
			m_wantKeyframe = true;
		}
		break;
	}
	default:
		break;
	}
}

void vncVideoStreamer::StopThread()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_stop = true;
	}
	m_cond.notify_all();
	if (m_thread.joinable())
		m_thread.join();
}

void vncVideoStreamer::Shutdown()
{
	std::lock_guard<std::mutex> control(m_controlMutex);
	StopThread();
	m_external = false;
}

bool vncVideoStreamer::BeginExternal()
{
	Start start;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (!m_haveStart)
			return false;
		start = m_lastStart;
	}
	std::lock_guard<std::mutex> control(m_controlMutex);
	StopThread(); // the monitor stream, if any
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_stop = false;
		m_wantKeyframe = false;
	}
	{
		std::lock_guard<std::mutex> lk(m_frameMutex);
		m_frameReady = false;
	}
	m_external = true;
	m_thread = std::thread(&vncVideoStreamer::ExternalThreadMain, this, start);
	return true;
}

void vncVideoStreamer::EndExternal()
{
	std::lock_guard<std::mutex> control(m_controlMutex);
	if (!m_external)
		return;
	StopThread();
	m_external = false;
}

void vncVideoStreamer::PushFrame(const uint8_t* bgra, int width, int height, int stride)
{
	if (width <= 0 || height <= 0)
		return;
	std::lock_guard<std::mutex> lk(m_frameMutex);
	m_framePixels.resize(size_t(width) * height * 4);
	for (int y = 0; y < height; ++y)
		memcpy(&m_framePixels[size_t(y) * width * 4], bgra + size_t(y) * stride, size_t(width) * 4);
	m_frameWidth = width;
	m_frameHeight = height;
	m_frameReady = true;
}

void vncVideoStreamer::ThreadMain(Start start)
{
	const int fps = std::clamp<int>(start.maxFps, 1, 120);
	const uint32_t bitrateKbps = std::clamp<uint32_t>(start.maxBitrateKbps, kMinKbps, 200000);
	std::vector<uint8_t> candidates = CodecCandidates(start);
	uint8_t codec = candidates.front();
	const auto interval = std::chrono::microseconds(1000000 / fps);
	RateController rate(bitrateKbps, Clock::now());
	const auto logInfo = [this](const std::string& msg) { Log(1, msg); };

	timeBeginPeriod(1); // 1 ms sleep resolution for the frame pacing

	vncVideoCapture capture;
	vncNvencEncoder encoder;
	RECT area = {};
	uint32_t streamId = 0;
	uint32_t frameNo = 0;
	bool forceIdr = true;
	std::string error;

	// (Re)start capture and encoder for the current framebuffer area and
	// announce the new stream.
	uint32_t failCode = kStatusUnsupported;
	auto setup = [&]() -> bool {
		std::string why;
		if (const uint32_t code = m_cb.framebufferArea(&area, &why)) {
			error = why;
			failCode = code;
			return false;
		}
		failCode = kStatusUnsupported;
		if (!capture.Init(area, error))
			return false;
		if (!encoder.IsOpen() || encoder.Width() != (capture.Width() & ~1) ||
		    encoder.Height() != (capture.Height() & ~1)) {
			// Older GPUs have no AV1 / HEVC encoder: fall back.
			if (!OpenBestCodec(encoder, capture.Device(), capture.Width(), capture.Height(), fps, rate.Current(),
				    candidates, codec, error, logInfo))
				return false;
		}
		Config cfg;
		streamId = m_nextStreamId++;
		cfg.streamId = streamId;
		cfg.codec = codec;
		cfg.width = static_cast<uint16_t>(encoder.Width());
		cfg.height = static_cast<uint16_t>(encoder.Height());
		cfg.x = static_cast<int16_t>(capture.OutputRect().left - area.left);
		cfg.y = static_cast<int16_t>(capture.OutputRect().top - area.top);
		cfg.fps = static_cast<uint16_t>(fps);
		cfg.bitrateKbps = encoder.BitrateKbps();
		m_cb.send(kPrioControl, kConfig, EncodeConfig(cfg));
		frameNo = 0;
		forceIdr = true;
		Log(1, "streaming " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) + " at " +
			std::to_string(fps) + " fps, " + std::to_string(cfg.bitrateKbps) + " kbps, " + CodecName(codec));
		return true;
	};

	// The encoder is bound to the capture device; a new device needs a new
	// encoder.
	auto restart = [&]() -> bool {
		encoder.Close();
		const auto deadline = Clock::now() + kRecoverTimeout;
		for (;;) {
			if (setup())
				return true;
			std::unique_lock<std::mutex> lk(m_mutex);
			if (m_stop || Clock::now() > deadline)
				return false;
			m_cond.wait_for(lk, std::chrono::milliseconds(200));
		}
	};

	if (!setup()) {
		SendStatus(failCode, error);
		timeEndPeriod(1);
		return;
	}

	auto next = Clock::now();
	auto nextAreaCheck = next + kAreaCheckInterval;
	auto nextStats = next + kStatsInterval;
	uint32_t sent = 0, skipped = 0;
	uint64_t bytes = 0;

	for (;;) {
		{
			std::unique_lock<std::mutex> lk(m_mutex);
			m_cond.wait_until(lk, next, [&] { return m_stop; });
			if (m_stop)
				break;
		}
		const auto now = Clock::now();
		next += interval;
		if (next < now)
			next = now + interval; // fell behind: do not try to catch up

		// The viewer may have switched monitors or the server changed mode.
		if (now >= nextAreaCheck) {
			nextAreaCheck = now + kAreaCheckInterval;
			RECT current;
			std::string why;
			if (const uint32_t code = m_cb.framebufferArea(&current, &why)) {
				SendStatus(code, why);
				break;
			}
			if (!SameRect(current, area) && !restart()) {
				SendStatus(kStatusCaptureLost, error);
				break;
			}
		}
		if (now >= nextStats) {
			nextStats = now + kStatsInterval;
			Log(2, "sent " + std::to_string(sent) + " frames (" + std::to_string(bytes / 1024) +
				" KB), skipped " + std::to_string(skipped) + " in the last 10 s, " +
				std::to_string(rate.Current()) + " kbps, rtt " +
				std::to_string(m_cb.rttMicros ? m_cb.rttMicros() / 1000 : 0) + " ms");
			sent = skipped = 0;
			bytes = 0;
		}

		if (rate.Update(now, m_cb.rttMicros ? m_cb.rttMicros() : 0) && encoder.SetBitrate(rate.Target())) {
			Log(2, std::string(rate.LastWasCongestion() ? "congestion" : "probing") + ": " +
				std::to_string(rate.Current()) + " -> " + std::to_string(rate.Target()) + " kbps");
			rate.Accept();
		}

		// Network behind: drop this frame before encoding.
		if (m_cb.queuedVideoBytes && m_cb.queuedVideoBytes() > QueueBudget(rate.Current(), fps)) {
			++skipped;
			rate.FrameSkipped();
			continue;
		}

		vncVideoCapture::Result r = capture.Acquire(0);
		if (r == vncVideoCapture::Result::Lost || r == vncVideoCapture::Result::Error) {
			Log(1, "capture lost (desktop switch or mode change), restarting");
			if (!restart()) {
				SendStatus(kStatusCaptureLost, error);
				break;
			}
			continue;
		}

		video::Cursor cursor;
		if (capture.TakePointerUpdate(cursor))
			m_cb.send(kPrioControl, kCursor, EncodeCursor(cursor));

		bool idr = forceIdr;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			idr = idr || m_wantKeyframe;
			m_wantKeyframe = false;
		}
		// A static screen produces no frames at all; the viewer keeps the
		// last picture. A requested keyframe re-encodes the last picture.
		if (r != vncVideoCapture::Result::Frame && !(idr && capture.HasPicture()))
			continue;

		std::vector<uint8_t> bitstream;
		bool key = false;
		const uint64_t t = NowUs();
		if (!encoder.Encode(capture.Texture(), idr, t, bitstream, key, error)) {
			SendStatus(kStatusEncoderError, error);
			break;
		}
		forceIdr = false;
		if (bitstream.empty())
			continue;

		FrameHeader fh;
		fh.streamId = streamId;
		fh.frameNo = frameNo++;
		fh.captureTimeUs = t;
		fh.flags = key ? kFrameKey : 0;
		if (!m_cb.send(kPrioVideo, kFrame, EncodeFrame(fh, bitstream.data(), bitstream.size())))
			break; // channel closed
		++sent;
		bytes += bitstream.size();
	}

	encoder.Close();
	capture.Release();
	timeEndPeriod(1);
}

// Single window as video: encode the canvases pushed by the window session.
// Same pacing, rate control, back pressure and keyframe handling as the
// monitor stream; the picture always starts at the framebuffer origin (the
// viewer's framebuffer is the window canvas).
void vncVideoStreamer::ExternalThreadMain(Start start)
{
	const int fps = std::clamp<int>(start.maxFps, 1, 120);
	const uint32_t bitrateKbps = std::clamp<uint32_t>(start.maxBitrateKbps, kMinKbps, 200000);
	std::vector<uint8_t> candidates = CodecCandidates(start);
	uint8_t codec = candidates.front();
	const auto interval = std::chrono::microseconds(1000000 / fps);
	RateController rate(bitrateKbps, Clock::now());
	const auto logInfo = [this](const std::string& msg) { Log(1, msg); };
	std::string error;

	auto fail = [&](uint32_t code, const std::string& message) {
		SendStatus(code, message);
		if (m_cb.onExternalFailed)
			m_cb.onExternalFailed();
	};

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	if (!CreateNvidiaDevice(&device, &context, error)) {
		fail(kStatusUnsupported, error);
		return;
	}
	timeBeginPeriod(1);

	vncNvencEncoder encoder;
	ID3D11Texture2D* texture = nullptr; // the canvas on the GPU
	std::vector<uint8_t> pixels;
	int width = 0, height = 0;
	bool pending = false;     // pixels not yet encoded
	bool havePicture = false; // texture holds a picture
	bool forceIdr = true;
	uint32_t streamId = 0, frameNo = 0;
	auto next = Clock::now();

	for (;;) {
		{
			std::unique_lock<std::mutex> lk(m_mutex);
			m_cond.wait_until(lk, next, [&] { return m_stop; });
			if (m_stop)
				break;
		}
		const auto now = Clock::now();
		next += interval;
		if (next < now)
			next = now + interval;

		if (rate.Update(now, m_cb.rttMicros ? m_cb.rttMicros() : 0) && encoder.IsOpen() &&
		    encoder.SetBitrate(rate.Target())) {
			Log(2, std::string(rate.LastWasCongestion() ? "congestion" : "probing") + ": " +
				std::to_string(rate.Current()) + " -> " + std::to_string(rate.Target()) + " kbps");
			rate.Accept();
		}

		{
			std::lock_guard<std::mutex> lk(m_frameMutex);
			if (m_frameReady) {
				pixels.swap(m_framePixels);
				width = m_frameWidth;
				height = m_frameHeight;
				m_frameReady = false;
				pending = true;
			}
		}
		bool idr = forceIdr;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			idr = idr || m_wantKeyframe;
			m_wantKeyframe = false;
		}
		if (!pending && !(idr && havePicture))
			continue; // nothing changed

		if (m_cb.queuedVideoBytes && m_cb.queuedVideoBytes() > QueueBudget(rate.Current(), fps)) {
			rate.FrameSkipped();
			if (idr) {
				std::lock_guard<std::mutex> lk(m_mutex);
				m_wantKeyframe = true; // keep the request
			}
			continue; // the pending picture is sent later (or a newer one)
		}

		if (pending) {
			const int w = width & ~1, h = height & ~1; // 4:2:0 needs even sizes
			if (w < 16 || h < 16) {
				pending = false;
				continue;
			}
			if (!encoder.IsOpen() || encoder.Width() != w || encoder.Height() != h) {
				encoder.Close();
				if (texture) {
					texture->Release();
					texture = nullptr;
				}
				D3D11_TEXTURE2D_DESC td = {};
				td.Width = w;
				td.Height = h;
				td.MipLevels = 1;
				td.ArraySize = 1;
				td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				td.SampleDesc.Count = 1;
				td.Usage = D3D11_USAGE_DEFAULT;
				td.BindFlags = D3D11_BIND_RENDER_TARGET;
				if (FAILED(device->CreateTexture2D(&td, NULL, &texture))) {
					fail(kStatusEncoderError, "CreateTexture2D for the window video failed");
					break;
				}
				if (!OpenBestCodec(encoder, device, w, h, fps, rate.Current(), candidates, codec, error, logInfo)) {
					fail(kStatusEncoderError, error);
					break;
				}
				Config cfg;
				streamId = m_nextStreamId++;
				cfg.streamId = streamId;
				cfg.codec = codec;
				cfg.width = static_cast<uint16_t>(w);
				cfg.height = static_cast<uint16_t>(h);
				cfg.x = 0;
				cfg.y = 0;
				cfg.fps = static_cast<uint16_t>(fps);
				cfg.bitrateKbps = encoder.BitrateKbps();
				m_cb.send(kPrioControl, kConfig, EncodeConfig(cfg));
				Log(1, "window video " + std::to_string(w) + "x" + std::to_string(h) + ", " +
					std::to_string(cfg.bitrateKbps) + " kbps, " + CodecName(codec));
				frameNo = 0;
				idr = true;
			}
			D3D11_BOX box = { 0, 0, 0, static_cast<UINT>(w), static_cast<UINT>(h), 1 };
			context->UpdateSubresource(texture, 0, &box, pixels.data(), static_cast<UINT>(width) * 4, 0);
			havePicture = true;
			pending = false;
		}

		std::vector<uint8_t> bitstream;
		bool key = false;
		const uint64_t t = NowUs();
		if (!encoder.Encode(texture, idr, t, bitstream, key, error)) {
			fail(kStatusEncoderError, error);
			break;
		}
		forceIdr = false;
		if (bitstream.empty())
			continue;
		FrameHeader fh;
		fh.streamId = streamId;
		fh.frameNo = frameNo++;
		fh.captureTimeUs = t;
		fh.flags = key ? kFrameKey : 0;
		if (!m_cb.send(kPrioVideo, kFrame, EncodeFrame(fh, bitstream.data(), bitstream.size())))
			break;
	}

	encoder.Close();
	if (texture)
		texture->Release();
	context->Release();
	device->Release();
	timeEndPeriod(1);
}
