// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Single window remote control, server side (docs/design.md section 4).
// Handles extension channel 2: lists windows, runs one capture thread for
// the selected window and sends the changed tiles of its canvas; maps the
// viewer's pointer events from canvas to screen coordinates and lets input
// reach only that window and its pop-ups.

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
#include "common/WindowProto.h"

class vncWindowSession {
public:
	struct Callbacks {
		std::function<bool(extmsg::ExtPriority, uint8_t subtype, const std::vector<uint8_t>&)> send;
		std::function<void(int level, const std::string&)> log;
		// Called on the capture thread when a session starts, after Active()
		// became true and before anything is sent: drop pending desktop
		// update requests so no desktop update follows the window canvas.
		std::function<void()> onSessionStart;
		// Tests only: also list and show windows of this process.
		bool allowOwnProcess = false;

		// Video transport (phase 3b, video mode on). beginVideo starts the
		// encoder for pushed frames and returns false if it cannot (then
		// tiles are used); pushVideo hands over a changed canvas; endVideo
		// stops the encoder.
		std::function<bool()> beginVideo;
		std::function<void(const uint8_t* bgra, int width, int height, int stride)> pushVideo;
		std::function<void()> endVideo;
	};

	explicit vncWindowSession(Callbacks cb);
	~vncWindowSession();

	vncWindowSession(const vncWindowSession&) = delete;
	vncWindowSession& operator=(const vncWindowSession&) = delete;

	// A complete message on channel kChWindow (transport reader thread).
	void OnMessage(uint8_t subtype, std::vector<uint8_t>& payload);
	void Shutdown();

	// A window is being shown: pointer input must go through HandlePointer.
	bool Active() const { return m_active.load(); }

	// Client thread, after the caller checked that this client may use the
	// mouse. Coordinates are canvas coordinates. wheelDelta (WHEEL_DELTA
	// units) is used for precise wheel messages, axis 0 vertical, 1
	// horizontal.
	void HandlePointer(int x, int y, uint8_t buttonMask, int wheelAxis, int wheelDelta);
	// Client thread, before a key is injected: make sure the window (or one
	// of its pop-ups) has the keyboard focus.
	void BeforeKey();

	// The video transport failed (any thread): continue with tiles.
	void VideoFailed() { m_videoFailed = true; }

private:
	void Select(uint64_t handle, uint8_t transport);
	void StopThread();
	void ThreadMain(HWND target, uint32_t sessionId, bool wantVideo);
	void SendState(uint32_t sessionId, uint8_t state, const std::string& message);
	void SendList();
	void Log(int level, const std::string& msg);

	Callbacks m_cb;
	std::mutex m_mutex;              // thread control and the input mapping
	std::condition_variable m_cond;
	std::thread m_thread;
	bool m_stop = false;
	std::atomic<bool> m_active{false};
	std::atomic<uint32_t> m_sessionId{0};
	std::atomic<uint32_t> m_lastAck{0};
	std::atomic<bool> m_videoFailed{false};
	uint32_t m_nextSessionId = 1;

	// Published by the capture thread for input (guarded by m_mutex).
	HWND m_target = NULL;
	POINT m_origin = {};
	std::vector<RECT> m_parts;

	// Pointer state (client thread only).
	uint8_t m_buttons = 0;
};
