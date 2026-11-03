// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Remote audio, server side (docs/design.md section 5): one thread per
// client records what the default playback device plays (WASAPI loopback),
// encodes it as AAC and sends it on extension channel 3. Optionally mutes
// the speakers while streaming and restores them afterwards.

#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/AudioProto.h"
#include "common/ExtChannel.h"

class vncAudioStreamer {
public:
	struct Callbacks {
		std::function<bool(extmsg::ExtPriority, uint8_t subtype, const std::vector<uint8_t>&)> send;
		std::function<void(int level, const std::string&)> log;
	};

	explicit vncAudioStreamer(Callbacks cb);
	~vncAudioStreamer();

	vncAudioStreamer(const vncAudioStreamer&) = delete;
	vncAudioStreamer& operator=(const vncAudioStreamer&) = delete;

	// A complete message on channel kChAudio (transport reader thread).
	void OnMessage(uint8_t subtype, std::vector<uint8_t>& payload);
	void Shutdown();

private:
	void ThreadMain(audio::Start start);
	void StopThread();
	void SendStatus(uint32_t code, const std::string& message);
	void Log(int level, const std::string& msg);

	Callbacks m_cb;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::thread m_thread;
	bool m_stop = false;
	uint32_t m_nextStreamId = 1;
};
