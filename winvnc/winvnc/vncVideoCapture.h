// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Screen capture for video mode: DXGI Desktop Duplication of one monitor.
// The captured picture stays in GPU memory (ID3D11Texture2D, BGRA) so that
// the hardware encoder can read it without a copy through system memory.
// Independent of the classic capture path (ddengine / mirror driver / GDI).

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <string>
#include <vector>

#include "common/VideoProto.h"

class vncVideoCapture {
public:
	enum class Result { Frame, Timeout, Lost, Error };

	vncVideoCapture() = default;
	~vncVideoCapture() { Release(); }

	vncVideoCapture(const vncVideoCapture&) = delete;
	vncVideoCapture& operator=(const vncVideoCapture&) = delete;

	// Duplicate the monitor that overlaps `screenArea` (screen coordinates)
	// the most. The monitor must be driven by an NVIDIA adapter, because the
	// encoder works on the same D3D11 device. Call again after Lost.
	bool Init(const RECT& screenArea, std::string& error);
	void Release();

	// Wait up to timeoutMs for a new desktop picture. On Frame the picture
	// is in Texture(); on Timeout the previous picture is still there.
	Result Acquire(UINT timeoutMs);

	ID3D11Device* Device() const { return m_device; }
	ID3D11DeviceContext* Context() const { return m_context; }
	ID3D11Texture2D* Texture() const { return m_frame; }
	bool HasPicture() const { return m_hasPicture; }
	// Monitor rectangle in screen coordinates.
	const RECT& OutputRect() const { return m_outputRect; }
	int Width() const { return m_outputRect.right - m_outputRect.left; }
	int Height() const { return m_outputRect.bottom - m_outputRect.top; }

	// True if some DXGI adapter is an NVIDIA GPU.
	static bool HasNvidiaAdapter();

	// The mouse pointer shape or visibility changed since the last call:
	// fills `out` (kCursorHidden when the pointer is not shown) and returns
	// true. The picture itself never contains the pointer.
	bool TakePointerUpdate(video::Cursor& out);

private:
	bool AttachToInputDesktop();

	ID3D11Device* m_device = nullptr;
	ID3D11DeviceContext* m_context = nullptr;
	IDXGIOutputDuplication* m_dup = nullptr;
	ID3D11Texture2D* m_frame = nullptr;   // our copy of the last picture
	RECT m_outputRect = {};
	bool m_hasPicture = false;
	HDESK m_desktop = NULL;

	// Mouse pointer.
	bool m_pointerDirty = false;
	bool m_pointerVisible = false;
	bool m_pointerVisibleKnown = false;
	bool m_hasShape = false;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO m_shapeInfo = {};
	std::vector<uint8_t> m_shape;
};
