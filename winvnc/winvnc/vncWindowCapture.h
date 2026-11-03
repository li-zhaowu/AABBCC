// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Capture of one window and its pop-ups (docs/design.md section 4.2).
//
// The window itself comes from Windows.Graphics.Capture, which also works
// when other windows cover it; where WGC is not available (or refuses, e.g.
// for some processes) PrintWindow(PW_RENDERFULLCONTENT) is used instead.
// Menus, combo box lists, tooltips and dialogs of the same program are
// separate top level windows; they are on top anyway, so they are copied
// from the screen and composited over the window picture. The result is one
// BGRA picture in system memory, the "canvas".

#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

class vncWindowCapture {
public:
	enum class Result { Ok, Minimized, Closed, Error };

	vncWindowCapture();
	~vncWindowCapture();

	vncWindowCapture(const vncWindowCapture&) = delete;
	vncWindowCapture& operator=(const vncWindowCapture&) = delete;

	// Call on the capture thread, which must be on the input desktop.
	bool Start(HWND target, std::string& error);
	void Stop();

	// Build the canvas for the current state of the window.
	Result Capture();

	// Canvas (valid after Capture() returned Ok).
	int Width() const { return m_canvasRect.right - m_canvasRect.left; }
	int Height() const { return m_canvasRect.bottom - m_canvasRect.top; }
	int Stride() const { return Width() * 4; }
	const uint8_t* Pixels() const { return m_canvas.data(); }
	// Canvas top left corner in screen coordinates.
	POINT Origin() const { return POINT{ m_canvasRect.left, m_canvasRect.top }; }
	// Window and pop-up rectangles in screen coordinates.
	const std::vector<RECT>& Parts() const { return m_parts; }
	HWND Target() const { return m_target; }
	bool UsesWgc() const;

	// The window and its pop-ups as top level windows (for input checks).
	static bool BelongsTo(HWND hwnd, HWND target);

private:
	struct Wgc;
	bool CaptureMain();
	bool CaptureMainWithPrintWindow();
	void FindPopups(std::vector<HWND>& popups);

	HWND m_target = NULL;
	std::unique_ptr<Wgc> m_wgc;      // null: PrintWindow fallback
	std::vector<uint8_t> m_main;     // last picture of the window itself
	int m_mainWidth = 0;
	int m_mainHeight = 0;
	RECT m_mainRect = {};            // where it is on the screen
	RECT m_canvasRect = {};
	std::vector<uint8_t> m_canvas;
	std::vector<RECT> m_parts;
};
