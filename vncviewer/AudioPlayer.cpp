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
constexpr auto kRetryInterval = std::chrono::seconds(1);

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
	m_open = true;
	m_thread = std::thread(&AudioPlayer::RenderThread, this, rate, channels);
	return true;
}

