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
		// Smoothed round-trip time of the connection in microseconds (0 =
		// not measured yet). Rising RTT means data piles up in the network.
		std::function<uint32_t()> rttMicros;
		// The framebuffer the viewer sees, in screen coordinates. Returns 0,
		// or a video::StatusCode if video mode cannot be used (for example
		// kStatusScaling while server side scaling is active).
		std::function<uint32_t(RECT* area, std::string* why)> framebufferArea;
		// The external (window) stream failed and was stopped; the window
		// session should fall back to lossless tiles.
		std::function<void()> onExternalFailed;
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

	// Single window as video (phase 3b). Instead of the monitor, frames
	// pushed by the window session are encoded, with the parameters of the
	// viewer's last Start. BeginExternal fails if the viewer never sent one.
	bool BeginExternal();
	// Newest canvas (BGRA). Copied; any thread.
	void PushFrame(const uint8_t* bgra, int width, int height, int stride);
	void EndExternal();

private:
	void ThreadMain(video::Start start);
	void ExternalThreadMain(video::Start start);
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

	// Serialises starting and stopping the thread (reader thread and window
	// session thread both do it).
	std::mutex m_controlMutex;
	bool m_external = false;           // guarded by m_controlMutex
	video::Start m_lastStart;          // guarded by m_mutex
	bool m_haveStart = false;          // guarded by m_mutex

	// Mailbox for external frames (newest wins).
	std::mutex m_frameMutex;
	std::vector<uint8_t> m_framePixels;
	int m_frameWidth = 0;
	int m_frameHeight = 0;
	bool m_frameReady = false;
};
