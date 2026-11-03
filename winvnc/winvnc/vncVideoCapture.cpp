// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "vncVideoCapture.h"

#include <dxgi1_5.h>

#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace {

const UINT kNvidiaVendorId = 0x10DE;

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

LONG Overlap(const RECT& a, const RECT& b)
{
	RECT r;
	if (!IntersectRect(&r, &a, &b))
		return 0;
	return (r.right - r.left) * (r.bottom - r.top);
}

} // namespace

bool vncVideoCapture::HasNvidiaAdapter()
{
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
		return false;
	bool found = false;
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == kNvidiaVendorId)
			found = true;
		adapter->Release();
	}
	factory->Release();
	return found;
}

bool vncVideoCapture::AttachToInputDesktop()
{
	// Desktop Duplication only works for the desktop the user currently
	// sees (Default, or Winlogon for UAC prompts and the lock screen).
	HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
	if (!input)
		return false;
	if (!SetThreadDesktop(input)) {
		CloseDesktop(input);
		return false;
	}
	if (m_desktop)
		CloseDesktop(m_desktop);
	m_desktop = input;
	return true;
}

bool vncVideoCapture::Init(const RECT& screenArea, std::string& error)
{
	Release();
	AttachToInputDesktop(); // best effort; fails harmlessly when already there

	IDXGIFactory1* factory = nullptr;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
	if (FAILED(hr)) {
		error = Hr("CreateDXGIFactory1", hr);
		return false;
	}

	// Find the output that shows most of the requested area.
	IDXGIAdapter1* bestAdapter = nullptr;
	IDXGIOutput* bestOutput = nullptr;
	LONG bestOverlap = 0;
	IDXGIAdapter1* adapter = nullptr;
	for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
		IDXGIOutput* output = nullptr;
		for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
			DXGI_OUTPUT_DESC od;
			const LONG ov = SUCCEEDED(output->GetDesc(&od)) && od.AttachedToDesktop
				? Overlap(od.DesktopCoordinates, screenArea) : 0;
			if (ov > bestOverlap) {
				SafeRelease(bestOutput);
				SafeRelease(bestAdapter);
				bestOverlap = ov;
				bestOutput = output;
				bestAdapter = adapter;
				bestAdapter->AddRef();
			} else {
				output->Release();
			}
		}
		adapter->Release();
	}
	factory->Release();
	if (!bestOutput) {
		error = "no monitor found for the viewed screen area";
		return false;
	}

	DXGI_ADAPTER_DESC1 ad;
	bestAdapter->GetDesc1(&ad);
	if (ad.VendorId != kNvidiaVendorId) {
		error = "the monitor is not driven by an NVIDIA GPU";
		SafeRelease(bestOutput);
		SafeRelease(bestAdapter);
		return false;
	}

	DXGI_OUTPUT_DESC od;
	bestOutput->GetDesc(&od);
	m_outputRect = od.DesktopCoordinates;

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	hr = D3D11CreateDevice(bestAdapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, NULL, &m_context);
	SafeRelease(bestAdapter);
	if (FAILED(hr)) {
		SafeRelease(bestOutput);
		error = Hr("D3D11CreateDevice", hr);
		return false;
	}

	// Ask for BGRA even on HDR monitors (DuplicateOutput1, Windows 10
	// 1703+); fall back to the older call.
	IDXGIOutput5* out5 = nullptr;
	hr = E_NOINTERFACE;
	if (SUCCEEDED(bestOutput->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&out5)))) {
		const DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
		hr = out5->DuplicateOutput1(m_device, 0, ARRAYSIZE(formats), formats, &m_dup);
		out5->Release();
	}
	if (FAILED(hr)) {
		IDXGIOutput1* out1 = nullptr;
		hr = bestOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&out1));
		if (SUCCEEDED(hr)) {
			hr = out1->DuplicateOutput(m_device, &m_dup);
			out1->Release();
		}
	}
	SafeRelease(bestOutput);
	if (FAILED(hr)) {
		error = Hr("DuplicateOutput", hr);
		Release();
		return false;
	}

	DXGI_OUTDUPL_DESC dd;
	m_dup->GetDesc(&dd);
	if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
		error = "the desktop format is not BGRA";
		Release();
		return false;
	}
	if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
		error = "rotated monitors are not supported in video mode";
		Release();
		return false;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = Width();
	td.Height = Height();
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	hr = m_device->CreateTexture2D(&td, NULL, &m_frame);
	if (FAILED(hr)) {
		error = Hr("CreateTexture2D", hr);
		Release();
		return false;
	}
	m_hasPicture = false;
	return true;
}

void vncVideoCapture::Release()
{
	SafeRelease(m_frame);
	SafeRelease(m_dup);
	SafeRelease(m_context);
	SafeRelease(m_device);
	m_hasPicture = false;
	// A new duplication reports the current pointer again.
	m_pointerVisibleKnown = false;
	m_hasShape = false;
	if (m_desktop) {
		CloseDesktop(m_desktop);
		m_desktop = NULL;
	}
}

vncVideoCapture::Result vncVideoCapture::Acquire(UINT timeoutMs)
{
	if (!m_dup)
		return Result::Lost;
	DXGI_OUTDUPL_FRAME_INFO info;
	IDXGIResource* res = nullptr;
	HRESULT hr = m_dup->AcquireNextFrame(timeoutMs, &info, &res);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return Result::Timeout;
	if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_ACCESS_DENIED || hr == DXGI_ERROR_INVALID_CALL)
		return Result::Lost; // mode change, desktop switch (UAC, lock), ...
	if (FAILED(hr))
		return Result::Error;

	Result result = Result::Timeout;
	// LastPresentTime == 0: only the mouse pointer changed; the picture
	// itself (which does not contain the pointer) is unchanged.
	if (info.LastPresentTime.QuadPart != 0 || !m_hasPicture) {
		ID3D11Texture2D* tex = nullptr;
		if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
			m_context->CopyResource(m_frame, tex);
			tex->Release();
			m_hasPicture = true;
			result = Result::Frame;
		} else {
			result = Result::Error;
		}
	}
	// Pointer: visibility comes with every mouse update, the shape only when
	// it changed. Both must be read before ReleaseFrame().
	if (info.LastMouseUpdateTime.QuadPart != 0) {
		const bool visible = info.PointerPosition.Visible != FALSE;
		if (!m_pointerVisibleKnown || visible != m_pointerVisible) {
			m_pointerVisible = visible;
			m_pointerVisibleKnown = true;
			m_pointerDirty = true;
		}
	}
	if (info.PointerShapeBufferSize > 0) {
		m_shape.resize(info.PointerShapeBufferSize);
		UINT required = 0;
		if (SUCCEEDED(m_dup->GetFramePointerShape(info.PointerShapeBufferSize, m_shape.data(), &required,
				&m_shapeInfo))) {
			m_hasShape = true;
			m_pointerDirty = true;
		}
	}

	res->Release();
	m_dup->ReleaseFrame();
	return result;
}

bool vncVideoCapture::TakePointerUpdate(video::Cursor& out)
{
	if (!m_pointerDirty)
		return false;
	out = video::Cursor();
	if (!m_pointerVisible) {
		m_pointerDirty = false;
		out.type = video::kCursorHidden;
		return true;
	}
	if (!m_hasShape)
		return false; // visible, shape not known yet: wait for it

	m_pointerDirty = false;
	const UINT w = m_shapeInfo.Width;
	const UINT pitch = m_shapeInfo.Pitch;
	UINT h = m_shapeInfo.Height;
	if (m_shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
		h /= 2; // AND mask rows followed by XOR mask rows
	if (w == 0 || h == 0 || w > video::kMaxCursorSize || h > video::kMaxCursorSize)
		return false;
	out.width = static_cast<uint16_t>(w);
	out.height = static_cast<uint16_t>(h);
	out.hotX = static_cast<uint16_t>((std::min)(m_shapeInfo.HotSpot.x, static_cast<LONG>(w - 1)));
	out.hotY = static_cast<uint16_t>((std::min)(m_shapeInfo.HotSpot.y, static_cast<LONG>(h - 1)));

	if (m_shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
		// Kept as masks so that inverting pointers (text I-beam) still invert.
		out.type = video::kCursorMonochrome;
		out.pitch = pitch;
		out.data.assign(m_shape.begin(), m_shape.begin() + size_t(pitch) * h * 2);
		return true;
	}

	out.type = video::kCursorColor;
	out.pitch = w * 4;
	out.data.resize(size_t(out.pitch) * h);
	for (UINT y = 0; y < h; ++y) {
		const uint8_t* src = m_shape.data() + size_t(y) * pitch;
		uint8_t* dst = out.data.data() + size_t(y) * out.pitch;
		for (UINT x = 0; x < w; ++x) {
			uint8_t b = src[x * 4], g = src[x * 4 + 1], r = src[x * 4 + 2], a = src[x * 4 + 3];
			if (m_shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
				// Mask 0: replace the screen pixel. Mask 0xFF: XOR with it;
				// XOR with black changes nothing, other colours are shown
				// as they are (close enough for a pointer).
				if (a == 0)
					a = 0xFF;
				else
					a = (r | g | b) ? 0xFF : 0;
			}
			dst[x * 4] = b;
			dst[x * 4 + 1] = g;
			dst[x * 4 + 2] = r;
			dst[x * 4 + 3] = a;
		}
	}
	return true;
}
