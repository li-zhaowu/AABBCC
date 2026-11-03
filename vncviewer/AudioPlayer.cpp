// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "AudioPlayer.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#endif

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

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

constexpr auto kPollInterval = std::chrono::milliseconds(10);
constexpr auto kDeviceCheckInterval = std::chrono::seconds(2);
constexpr auto kRetryInterval = std::chrono::milliseconds(1000);

} // namespace

AudioPlayer::AudioPlayer(LogFn log) : m_log(std::move(log)) {}

AudioPlayer::~AudioPlayer()
{
	Close();
}

void AudioPlayer::Log(int level, const std::string& msg)
{
	if (m_log)
		m_log(level, "audio: " + msg);
}

bool AudioPlayer::Open(const audio::Format& format, std::string& error)
{
	Close();
	std::lock_guard<std::mutex> lk(m_controlMutex);
	if (format.codec != audio::kCodecAac) {
		error = "unknown audio codec";
		return false;
	}
	const int rate = static_cast<int>(format.sampleRate);
	const int channels = format.channels;
	if (!m_decoder.Open(rate, channels, format.config, error))
		return false;
	{
		std::lock_guard<std::mutex> blk(m_bufMutex);
		m_buffer = audio::JitterBuffer(channels, size_t(rate) * kTargetMs / 1000, size_t(rate) * kMaxMs / 1000);
		m_stop = false;
	}
	m_streamId = format.streamId;
	m_channels = channels;
	m_open = true;
	m_thread = std::thread(&AudioPlayer::RenderThread, this, rate, channels);
	return true;
}


void AudioPlayer::Push(uint32_t streamId, const uint8_t* frame, size_t size)
{
	std::lock_guard<std::mutex> lk(m_controlMutex);
	if (!m_open || streamId != m_streamId)
		return;
	m_pcm.clear();
	std::string error;
	if (!m_decoder.Decode(frame, size, m_pcm, error)) {
		Log(0, "decoder: " + error);
		return;
	}
	if (m_pcm.empty())
		return;
	std::lock_guard<std::mutex> blk(m_bufMutex);
	m_buffer.Push(m_pcm.data(), m_pcm.size() / m_channels);
}

void AudioPlayer::Close()
{
	std::lock_guard<std::mutex> lk(m_controlMutex);
	{
		std::lock_guard<std::mutex> blk(m_bufMutex);
		m_stop = true;
	}
	m_cond.notify_all();
	if (m_thread.joinable())
		m_thread.join();
	if (m_open) {
		uint64_t underruns = 0, dropped = 0;
		{
			std::lock_guard<std::mutex> blk(m_bufMutex);
			underruns = m_buffer.Underruns();
			dropped = m_buffer.DroppedFrames();
		}
		Log(1, "stream " + std::to_string(m_streamId) + " stopped, " + std::to_string(underruns) +
			" underrun(s), " + std::to_string(dropped) + " frame(s) dropped");
	}
	m_open = false;
	m_decoder.Close();
}

// Plays the jitter buffer on the default playback device. Device loss and
// a change of the default device reopen it; without any device the buffer
// just fills up to its maximum and old audio is dropped.
void AudioPlayer::RenderThread(int sampleRate, int channels)
{
	const HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	const UINT32 deviceTarget = static_cast<UINT32>(sampleRate * kDeviceMs / 1000);
	std::vector<int16_t> silence;
	std::string lastError;

	auto stopRequested = [&](std::chrono::milliseconds wait) {
		std::unique_lock<std::mutex> lk(m_bufMutex);
		m_cond.wait_for(lk, wait, [&] { return m_stop; });
		return m_stop;
	};

	for (;;) {
		IMMDeviceEnumerator* en = nullptr;
		IMMDevice* device = nullptr;
		IAudioClient* client = nullptr;
		IAudioRenderClient* render = nullptr;
		std::wstring deviceId;
		UINT32 bufferFrames = 0;
		std::string error;

		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&en));
		if (SUCCEEDED(hr))
			hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &device);
		if (FAILED(hr))
			error = "no playback device";
		if (SUCCEEDED(hr)) {
			LPWSTR id = nullptr;
			if (SUCCEEDED(device->GetId(&id))) {
				deviceId = id;
				CoTaskMemFree(id);
			}
			hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&client));
		}
		if (SUCCEEDED(hr)) {
			// 16 bit PCM at the stream's rate; Windows converts to the mix
			// format.
			WAVEFORMATEX wf{};
			wf.wFormatTag = WAVE_FORMAT_PCM;
			wf.nChannels = static_cast<WORD>(channels);
			wf.nSamplesPerSec = static_cast<DWORD>(sampleRate);
			wf.wBitsPerSample = 16;
			wf.nBlockAlign = static_cast<WORD>(channels * 2);
			wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
			hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
				2000000, 0, &wf, NULL); // 200 ms device buffer
		}
		if (SUCCEEDED(hr))
			hr = client->GetBufferSize(&bufferFrames);
		if (SUCCEEDED(hr))
			hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
		if (SUCCEEDED(hr))
			hr = client->Start();

		bool reopen = false;
		if (FAILED(hr)) {
			if (error.empty())
				error = Hr("audio playback", hr);
		} else {
			if (!lastError.empty())
				Log(1, "playback device available again");
			lastError.clear();
			Log(1, "playing " + std::to_string(sampleRate) + " Hz, " + std::to_string(channels) + " channel(s)");
			auto nextDeviceCheck = Clock::now() + kDeviceCheckInterval;
			while (!stopRequested(kPollInterval)) {
				UINT32 padding = 0;
				hr = client->GetCurrentPadding(&padding);
				if (SUCCEEDED(hr) && padding < deviceTarget) {
					const UINT32 n = (std::min)(deviceTarget - padding, bufferFrames - padding);
					BYTE* data = nullptr;
					if (n > 0 && SUCCEEDED(hr = render->GetBuffer(n, &data))) {
						{
							std::lock_guard<std::mutex> blk(m_bufMutex);
							m_buffer.Pull(reinterpret_cast<int16_t*>(data), n);
						}
						hr = render->ReleaseBuffer(n, 0);
					}
				}
				if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
					Log(1, "playback device went away");
					reopen = true;
					break;
				}
				if (FAILED(hr)) {
					error = Hr("audio playback", hr);
					break;
				}
				const auto now = Clock::now();
				if (now >= nextDeviceCheck) {
					nextDeviceCheck = now + kDeviceCheckInterval;
					IMMDevice* current = nullptr;
					LPWSTR id = nullptr;
					if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &current)) &&
					    SUCCEEDED(current->GetId(&id))) {
						reopen = deviceId != id;
						CoTaskMemFree(id);
					}
					SafeRelease(current);
					if (reopen) {
						Log(1, "default playback device changed");
						break;
					}
				}
			}
			client->Stop();
		}
		SafeRelease(render);
		SafeRelease(client);
		SafeRelease(device);
		SafeRelease(en);

		if (!error.empty() && error != lastError) {
			Log(0, error); // logged once until it changes
			lastError = error;
		}
		if (reopen) {
			if (stopRequested(std::chrono::milliseconds(0)))
				break;
			continue;
		}
		if (stopRequested(kRetryInterval))
			break;
		// No device: keep the buffer from holding stale audio.
		std::lock_guard<std::mutex> blk(m_bufMutex);
		m_buffer.Clear();
	}
	if (SUCCEEDED(hrCom))
		CoUninitialize();
}
