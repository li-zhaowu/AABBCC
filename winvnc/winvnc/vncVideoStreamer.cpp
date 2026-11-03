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

// Bitrate adaptation (AIMD): checked twice a second; on congestion the rate
// drops by 30 %, after 2 s without congestion it grows by 5 % of the
// maximum. Congestion = frames skipped because the send queue was full, or
// the round-trip time 150 ms above the lowest seen (data waiting in TCP
// buffers somewhere on the path).
constexpr auto kRateInterval = std::chrono::milliseconds(500);
constexpr auto kRateGrowDelay = std::chrono::seconds(2);
constexpr auto kBaseRttRefresh = std::chrono::seconds(30);
constexpr uint32_t kMinKbps = 1000;
constexpr uint32_t kRttMarginUs = 150000;

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
		StopThread();
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_stop = false;
			m_wantKeyframe = false;
		}
		m_thread = std::thread(&vncVideoStreamer::ThreadMain, this, start);
		break;
	}
	case kStop:
		StopThread();
		SendStatus(kStatusStopped, "");
		break;
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
	StopThread();
}

void vncVideoStreamer::ThreadMain(Start start)
{
	const int fps = std::clamp<int>(start.maxFps, 1, 120);
	const uint32_t bitrateKbps = std::clamp<uint32_t>(start.maxBitrateKbps, kMinKbps, 200000);
	uint8_t codec = start.codec == kCodecHevc ? kCodecHevc : kCodecH264;
	const auto interval = std::chrono::microseconds(1000000 / fps);
	// Start at half the maximum and probe upwards: over the internet the
	// maximum is often too much, and too much means lag.
	uint32_t currentKbps = (std::max)(kMinKbps, bitrateKbps / 2);
	// Skip encoding while more than about three frames wait to be sent.
	auto queueBudget = [&] { return std::max<size_t>(256 * 1024, size_t(currentKbps) * 1000 / 8 / fps * 3); };

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
			bool opened = encoder.Open(capture.Device(), capture.Width(), capture.Height(), fps, currentKbps,
				codec, error);
			if (!opened && codec == kCodecHevc) {
				// Older GPUs have no HEVC encoder: fall back to H.264.
				Log(1, "HEVC not available (" + error + "), using H.264");
				codec = kCodecH264;
				opened = encoder.Open(capture.Device(), capture.Width(), capture.Height(), fps, currentKbps,
					codec, error);
			}
			if (!opened)
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
			std::to_string(fps) + " fps, " + std::to_string(cfg.bitrateKbps) + " kbps, " +
			(codec == kCodecHevc ? "HEVC" : "H.264"));
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
	auto nextRateCheck = next + kRateInterval;
	auto lastDecrease = next;
	auto nextBaseRefresh = next + kBaseRttRefresh;
	uint32_t baseRtt = 0;
	uint32_t sent = 0, skipped = 0, skippedSinceRateCheck = 0;
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
				std::to_string(currentKbps) + " kbps, rtt " +
				std::to_string(m_cb.rttMicros ? m_cb.rttMicros() / 1000 : 0) + " ms");
			sent = skipped = 0;
			bytes = 0;
		}

		if (now >= nextRateCheck) {
			nextRateCheck = now + kRateInterval;
			const uint32_t rtt = m_cb.rttMicros ? m_cb.rttMicros() : 0;
			if (rtt != 0 && (baseRtt == 0 || rtt < baseRtt))
				baseRtt = rtt;
			const bool rttHigh = rtt != 0 && baseRtt != 0 && rtt > baseRtt + kRttMarginUs;
			const bool congested = skippedSinceRateCheck > 0 || rttHigh;
			skippedSinceRateCheck = 0;
			uint32_t target = currentKbps;
			if (congested) {
				target = (std::max)(kMinKbps, currentKbps * 7 / 10);
				lastDecrease = now;
			} else if (now - lastDecrease >= kRateGrowDelay) {
				target = (std::min)(bitrateKbps, currentKbps + std::max<uint32_t>(250, bitrateKbps / 20));
				// The route may have changed: re-learn the base RTT now and then,
				// but only while the link is calm.
				if (now >= nextBaseRefresh) {
					nextBaseRefresh = now + kBaseRttRefresh;
					baseRtt = rtt;
				}
			}
			if (target != currentKbps && encoder.SetBitrate(target)) {
				Log(2, std::string(congested ? "congestion" : "probing") + ": " + std::to_string(currentKbps) +
					" -> " + std::to_string(target) + " kbps");
				currentKbps = target;
			}
		}

		// Network behind: drop this frame before encoding.
		if (m_cb.queuedVideoBytes && m_cb.queuedVideoBytes() > queueBudget()) {
			++skipped;
			++skippedSinceRateCheck;
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
