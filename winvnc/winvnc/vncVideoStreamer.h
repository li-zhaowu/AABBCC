// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Video mode, server side (docs/design.md section 3): one thread per
// client captures the monitor the viewer looks at with DXGI Desktop
// Duplication, encodes it with NVENC and sends the frames on extension
// channel 4. When the network falls behind, frames are skipped before
// encoding (never after), so the stream never breaks.

#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/ExtChannel.h"
#include "common/VideoProto.h"

class vncVideoStreamer {
public:
	struct Callbacks {
		std::function<bool(extmsg::ExtPriority, uint8_t subtype, const std::vector<uint8_t>&)> send;
		std::function<void(int level, const std::string&)> log;
		// Bytes of video waiting in the extension channel's send queue.
		std::function<size_t()> queuedVideoBytes;
		// The framebuffer the viewer sees, in screen coordinates. Returns 0,
		// or a video::StatusCode if video mode cannot be used (for example
		// kStatusScaling while server side scaling is active).
		std::function<uint32_t(RECT* area, std::string* why)> framebufferArea;
	};

	explicit vncVideoStreamer(Callbacks cb);
	~vncVideoStreamer();

	vncVideoStreamer(const vncVideoStreamer&) = delete;
	vncVideoStreamer& operator=(const vncVideoStreamer&) = delete;

	// NVENC usable and an NVIDIA GPU present. Cached after the first call.
	static bool IsSupported(std::string* why);

	// A complete message on channel kChVideo (transport reader thread).
	void OnMessage(uint8_t subtype, std::vector<uint8_t>& payload);

	// Stop streaming and join the thread (connection closing).
	void Shutdown();

private:
	void ThreadMain(video::Start start);
	void SendStatus(uint32_t code, const std::string& message);
	void StopThread();
	void Log(int level, const std::string& msg);

	Callbacks m_cb;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::thread m_thread;
	bool m_stop = false;
	bool m_wantKeyframe = false;
	std::atomic<uint32_t> m_nextStreamId{1};
};
