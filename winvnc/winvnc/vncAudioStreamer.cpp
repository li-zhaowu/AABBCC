// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "vncAudioStreamer.h"
#include "common/AacCodec.h"

#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <map>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#endif

using namespace audio;
using extmsg::kPrioAudio;
using extmsg::kPrioControl;
using Clock = std::chrono::steady_clock;

namespace {

template <class T>
void SafeRelease(T*& p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

std::string Hr(const char* what, HRESULT hr)
{
	char buf[96];
	snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
	return buf;
}

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT (no ksmedia.h needed).
const GUID kSubtypePcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

constexpr auto kPollInterval = std::chrono::milliseconds(10);
constexpr auto kDeviceCheckInterval = std::chrono::seconds(2);
constexpr int kMaxDeviceFailures = 5;

// Speakers muted while streaming. Several viewers may ask for it; the
// original state comes back when the last one stops.
struct MuteRegistry {
	std::mutex mutex;
	std::map<std::wstring, std::pair<int, BOOL>> devices; // id -> (users, original mute)
};

MuteRegistry& Mutes()
{
	static MuteRegistry r;
	return r;
}

class SpeakerMute {
public:
	~SpeakerMute() { Release(); }
	bool Apply(IMMDevice* device)
	{
		Release();
		LPWSTR id = nullptr;
		if (FAILED(device->GetId(&id)))
			return false;
		m_id = id;
		CoTaskMemFree(id);
		if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&m_volume))))
			return false;
		std::lock_guard<std::mutex> lk(Mutes().mutex);
		auto& entry = Mutes().devices[m_id];
		if (entry.first++ == 0)
			m_volume->GetMute(&entry.second);
		m_volume->SetMute(TRUE, NULL);
		return true;
	}
	void Release()
	{
		if (!m_volume)
			return;
		{
			std::lock_guard<std::mutex> lk(Mutes().mutex);
			auto it = Mutes().devices.find(m_id);
			if (it != Mutes().devices.end() && --it->second.first == 0) {
				m_volume->SetMute(it->second.second, NULL);
				Mutes().devices.erase(it);
			}
		}
		SafeRelease(m_volume);
	}

private:
	IAudioEndpointVolume* m_volume = nullptr;
	std::wstring m_id;
};

// Mix format -> what we can read.
struct SourceFormat {
	bool isFloat = false;
	int bits = 0;        // container bits per sample
	int channels = 0;
	int rate = 0;
	int blockAlign = 0;
	int outRate = 0;     // 44100 or 48000
	int decimate = 1;    // rate / outRate
};

bool Describe(const WAVEFORMATEX* f, SourceFormat& s, std::string& error)
{
	GUID sub = kSubtypePcm;
	if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		sub = kSubtypeFloat;
	else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
		sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat;
	else if (f->wFormatTag != WAVE_FORMAT_PCM)
		sub = GUID{};
	s.isFloat = sub == kSubtypeFloat;
	const bool isPcm = sub == kSubtypePcm;
	s.bits = f->wBitsPerSample;
	s.channels = f->nChannels;
	s.rate = static_cast<int>(f->nSamplesPerSec);
	s.blockAlign = f->nBlockAlign;
	if (!(s.isFloat && s.bits == 32) && !(isPcm && (s.bits == 16 || s.bits == 24 || s.bits == 32))) {
		error = "unsupported playback format";
		return false;
	}
	for (int base : { 48000, 44100 }) {
		for (int factor : { 1, 2, 4 }) {
			if (s.rate == base * factor) {
				s.outRate = base;
				s.decimate = factor;
				return s.channels >= 1;
			}
		}
	}
	error = "unsupported sample rate " + std::to_string(s.rate);
	return false;
}

float ReadSample(const BYTE* frame, int channel, const SourceFormat& s)
{
	const BYTE* p = frame + channel * (s.bits / 8);
	if (s.isFloat)
		return *reinterpret_cast<const float*>(p);
	if (s.bits == 16)
		return *reinterpret_cast<const int16_t*>(p) / 32768.0f;
	if (s.bits == 24) {
		const int32_t v = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 24);
		return v / 2147483648.0f;
	}
	return *reinterpret_cast<const int32_t*>(p) / 2147483648.0f;
}

int16_t ToInt16(float v)
{
	v = (std::max)(-1.0f, (std::min)(1.0f, v));
	return static_cast<int16_t>(v * 32767.0f);
}

} // namespace

vncAudioStreamer::vncAudioStreamer(Callbacks cb) : m_cb(std::move(cb)) {}

vncAudioStreamer::~vncAudioStreamer()
{
	Shutdown();
}

void vncAudioStreamer::Log(int level, const std::string& msg)
{
	if (m_cb.log)
		m_cb.log(level, "audio: " + msg);
}

void vncAudioStreamer::SendStatus(uint32_t code, const std::string& message)
{
	Status st;
	st.code = code;
	st.message = message;
	m_cb.send(kPrioControl, kStatus, EncodeStatus(st));
	if (code != kStatusStopped)
		Log(0, "stopped: " + message);
}

void vncAudioStreamer::OnMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	if (subtype == kStart) {
		Start start;
		if (!DecodeStart(payload.data(), payload.size(), start) || start.codec != kCodecAac) {
			SendStatus(kStatusUnsupported, "unsupported audio request");
			return;
		}
		StopThread();
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_stop = false;
		}
		m_thread = std::thread(&vncAudioStreamer::ThreadMain, this, start);
	} else if (subtype == kStop) {
		StopThread();
		SendStatus(kStatusStopped, "");
	}
}

void vncAudioStreamer::StopThread()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_stop = true;
	}
	m_cond.notify_all();
	if (m_thread.joinable())
		m_thread.join();
}

void vncAudioStreamer::Shutdown()
{
	StopThread();
}

void vncAudioStreamer::ThreadMain(Start start)
{
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	const bool mute = (start.flags & kStartMuteRemote) != 0;
	int failures = 0;
	std::string error;
	uint32_t failCode = kStatusDeviceLost;

	auto stopRequested = [&](std::chrono::milliseconds wait) {
		std::unique_lock<std::mutex> lk(m_mutex);
		m_cond.wait_for(lk, wait, [&] { return m_stop; });
		return m_stop;
	};

	// One pass per playback device; returns true when the device changed
	// (go on with the new one), false on stop or failure.
	auto runDevice = [&]() -> bool {
		IMMDeviceEnumerator* en = nullptr;
		IMMDevice* device = nullptr;
		IAudioClient* client = nullptr;
		IAudioCaptureClient* capture = nullptr;
		WAVEFORMATEX* mix = nullptr;
		SpeakerMute speakerMute;
		AacEncoder encoder;
		bool changed = false;
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&en));
		if (SUCCEEDED(hr))
			hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &device);
		if (FAILED(hr)) {
			error = "no playback device";
			failCode = kStatusUnsupported;
		}
		std::wstring deviceId;
		if (SUCCEEDED(hr)) {
			LPWSTR id = nullptr;
			if (SUCCEEDED(device->GetId(&id))) {
				deviceId = id;
				CoTaskMemFree(id);
			}
			hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&client));
		}
		if (SUCCEEDED(hr))
			hr = client->GetMixFormat(&mix);
		SourceFormat src;
		if (SUCCEEDED(hr) && !Describe(mix, src, error)) {
			hr = E_FAIL;
			failCode = kStatusUnsupported;
		}
		// Loopback: record what this device plays.
		if (SUCCEEDED(hr))
			hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, NULL);
		if (SUCCEEDED(hr))
			hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
		const int outChannels = SUCCEEDED(hr) ? (std::min)(2, src.channels) : 0;
		if (SUCCEEDED(hr) && !encoder.Open(src.outRate, outChannels, start.bitrateKbps, error)) {
			hr = E_FAIL;
			failCode = kStatusUnsupported;
		}
		if (SUCCEEDED(hr))
			hr = client->Start();
		if (FAILED(hr)) {
			if (error.empty())
				error = Hr("audio capture", hr);
		} else {
			Format fmt;
			fmt.streamId = m_nextStreamId++;
			fmt.channels = static_cast<uint8_t>(outChannels);
			fmt.sampleRate = static_cast<uint32_t>(src.outRate);
			fmt.bitrateKbps = encoder.BitrateKbps();
			fmt.config = encoder.Config();
			m_cb.send(kPrioAudio, kFormat, EncodeFormat(fmt));
			if (mute && !speakerMute.Apply(device))
				Log(0, "could not mute the speakers");
			Log(1, "streaming " + std::to_string(src.outRate) + " Hz, " + std::to_string(outChannels) + " channel(s), " +
				std::to_string(fmt.bitrateKbps) + " kbps AAC" + (mute ? ", speakers muted" : ""));
			failures = 0;

			std::vector<int16_t> pcm;
			std::vector<std::vector<uint8_t>> frames;
			uint32_t seq = 0;
			uint64_t produced = 0; // frames at outRate
			const auto started = Clock::now();
			auto nextDeviceCheck = started + kDeviceCheckInterval;
			int decimateLeft = 0;
			float acc[2] = { 0, 0 };

			while (!stopRequested(kPollInterval)) {
				// Recorded packets.
				UINT32 packet = 0;
				while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
					BYTE* data = nullptr;
					UINT32 count = 0;
					DWORD flags = 0;
					hr = capture->GetBuffer(&data, &count, &flags, NULL, NULL);
					if (FAILED(hr))
						break;
					const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
					for (UINT32 i = 0; i < count; ++i) {
						const BYTE* frame = data + size_t(i) * src.blockAlign;
						for (int c = 0; c < outChannels; ++c)
							acc[c] += silent ? 0.0f : ReadSample(frame, c, src);
						if (++decimateLeft == src.decimate) {
							for (int c = 0; c < outChannels; ++c) {
								pcm.push_back(ToInt16(acc[c] / src.decimate));
								acc[c] = 0;
							}
							decimateLeft = 0;
							++produced;
						}
					}
					capture->ReleaseBuffer(count);
				}
				if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
					Log(1, "playback device went away");
					changed = true;
					break;
				}
				if (FAILED(hr)) {
					error = Hr("audio capture", hr);
					break;
				}
				// Loopback delivers nothing while nothing plays: keep the
				// stream going with silence (about 20 ms behind real time).
				const auto now = Clock::now();
				const uint64_t expected = static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(now - started).count()) * src.outRate / 1000000;
				if (expected > produced + src.outRate / 20) {
					const uint64_t fill = expected - produced - src.outRate / 50;
					pcm.insert(pcm.end(), size_t(fill) * outChannels, int16_t(0));
					produced += fill;
				}
				if (!pcm.empty()) {
					frames.clear();
					if (!encoder.Encode(pcm.data(), pcm.size() / outChannels, frames, error)) {
						failCode = kStatusError;
						break;
					}
					pcm.clear();
					const uint64_t t = static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
					for (const auto& fr : frames) {
						DataHeader dh;
						dh.streamId = fmt.streamId;
						dh.seq = seq++;
						dh.timestampUs = t;
						if (!m_cb.send(kPrioAudio, kData, EncodeData(dh, fr.data(), fr.size()))) {
							std::lock_guard<std::mutex> lk(m_mutex);
							m_stop = true; // connection closed
						}
					}
				}
				// The user picked another playback device: follow it.
				if (now >= nextDeviceCheck) {
					nextDeviceCheck = now + kDeviceCheckInterval;
					IMMDevice* current = nullptr;
					LPWSTR id = nullptr;
					if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &current)) &&
					    SUCCEEDED(current->GetId(&id))) {
						changed = deviceId != id;
						CoTaskMemFree(id);
					}
					SafeRelease(current);
					if (changed) {
						Log(1, "default playback device changed");
						break;
					}
				}
			}
			client->Stop();
		}
		speakerMute.Release();
		CoTaskMemFree(mix);
		SafeRelease(capture);
		SafeRelease(client);
		SafeRelease(device);
		SafeRelease(en);
		return changed;
	};

	for (;;) {
		error.clear();
		if (runDevice())
			continue;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			if (m_stop)
				break;
		}
		if (++failures > kMaxDeviceFailures) {
			SendStatus(failCode, error);
			break;
		}
		if (stopRequested(std::chrono::milliseconds(1000)))
			break;
	}
	CoUninitialize();
}
