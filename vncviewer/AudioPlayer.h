// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Remote audio, viewer side (docs/design.md section 5): AAC frames from the
// server are decoded on the caller's thread into a jitter buffer; a render
// thread feeds the default playback device (WASAPI shared mode) from it.

#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/AacCodec.h"
#include "common/AudioProto.h"

class AudioPlayer {
public:
	using LogFn = std::function<void(int level, const std::string&)>;

	explicit AudioPlayer(LogFn log);
	~AudioPlayer();

	AudioPlayer(const AudioPlayer&) = delete;
	AudioPlayer& operator=(const AudioPlayer&) = delete;

	// A new stream: (re)start decoding and playback. Any thread.
	bool Open(const audio::Format& format, std::string& error);
	// One AAC frame of the current stream. Any thread.
	void Push(uint32_t streamId, const uint8_t* frame, size_t size);
	// Stop playback. Any thread except the render thread.
	void Close();

	// Buffering targets (frames at the stream's rate are derived from these).
	static constexpr int kTargetMs = 80;  // jitter buffer before playing
	static constexpr int kMaxMs = 300;    // beyond this, old audio is dropped
	static constexpr int kDeviceMs = 40;  // kept queued in the device

private:
	void RenderThread(int sampleRate, int channels);
	void Log(int level, const std::string& msg);

	LogFn m_log;

	// Lifecycle and decoder (Open / Push / Close).
	std::mutex m_controlMutex;
	audio::AacDecoder m_decoder;
	uint32_t m_streamId = 0;
	int m_channels = 0;
	bool m_open = false;
	std::vector<int16_t> m_pcm;
	std::thread m_thread;

	// Shared with the render thread.
	std::mutex m_bufMutex;
	std::condition_variable m_cond;
	bool m_stop = false;
	audio::JitterBuffer m_buffer{ 2, 0, 0 };
};
