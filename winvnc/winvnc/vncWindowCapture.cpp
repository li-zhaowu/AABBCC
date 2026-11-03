// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "vncWindowCapture.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>

// Windows.Graphics.Capture ABI. MSVC builds use the Windows SDK headers.
// MinGW (used for tests only) ships incomplete ones, so the few interfaces
// needed are declared here in the same vtable order as the SDK's IDL.
#if defined(_MSC_VER)
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.directx.direct3d11.h>
namespace wgc {
using Item = ABI::Windows::Graphics::Capture::IGraphicsCaptureItem;
using FramePool = ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using FramePoolStatics2 = ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using Frame = ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using Session = ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
using Session2 = ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2;
// IGraphicsCaptureSession3 (border control) needs SDK 10.0.22000 or later.
#ifdef ____x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession3_INTERFACE_DEFINED__
#define UVNC_HAVE_WGC_SESSION3 1
using Session3 = ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3;
#endif
using Device = ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using Surface = ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Closable = ABI::Windows::Foundation::IClosable;
using SizeInt32 = ABI::Windows::Graphics::SizeInt32;
const auto kFormatBgra = ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;
template <class T> const IID& Iid() { return __uuidof(T); }
} // namespace wgc
#else
namespace wgc {
struct SizeInt32 { INT32 Width; INT32 Height; };
typedef int PixelFormat;
const PixelFormat kFormatBgra = 87; // DirectXPixelFormat_B8G8R8A8UIntNormalized
struct Device : public IInspectable { virtual HRESULT STDMETHODCALLTYPE Trim() = 0; };
struct Surface : public IInspectable { virtual HRESULT STDMETHODCALLTYPE get_Description(void*) = 0; };
struct Closable : public IInspectable { virtual HRESULT STDMETHODCALLTYPE Close() = 0; };
struct Item : public IInspectable {
	virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING*) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32*) = 0;
	virtual HRESULT STDMETHODCALLTYPE add_Closed(void*, INT64*) = 0;
	virtual HRESULT STDMETHODCALLTYPE remove_Closed(INT64) = 0;
};
struct Session : public IInspectable { virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0; };
struct Session2 : public IInspectable {
	virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(unsigned char*) = 0;
	virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(unsigned char) = 0;
};
#define UVNC_HAVE_WGC_SESSION3 1
struct Session3 : public IInspectable {
	virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(unsigned char*) = 0;
	virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(unsigned char) = 0;
};
struct Frame : public IInspectable {
	virtual HRESULT STDMETHODCALLTYPE get_Surface(Surface**) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(INT64*) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32*) = 0;
};
struct FramePool : public IInspectable {
	virtual HRESULT STDMETHODCALLTYPE Recreate(Device*, PixelFormat, INT32, SizeInt32) = 0;
	virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(Frame**) = 0;
	virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void*, INT64*) = 0;
	virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(INT64) = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(Item*, Session**) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(void**) = 0;
};
struct FramePoolStatics2 : public IInspectable {
	virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(Device*, PixelFormat, INT32, SizeInt32, FramePool**) = 0;
};
template <class T> const IID& Iid();
#define WGC_IID(T, l, w1, w2, b0, b1, b2, b3, b4, b5, b6, b7) \
	template <> inline const IID& Iid<T>() { static const IID iid = { l, w1, w2, { b0, b1, b2, b3, b4, b5, b6, b7 } }; return iid; }
WGC_IID(Item, 0x79c3f95b, 0x31f7, 0x4ec2, 0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60)
WGC_IID(FramePool, 0x24eb6d22, 0x1975, 0x422e, 0x82, 0xe7, 0x78, 0x0d, 0xbd, 0x8d, 0xdf, 0x24)
WGC_IID(FramePoolStatics2, 0x589b103f, 0x6bbc, 0x5df5, 0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5)
WGC_IID(Frame, 0xfa50c623, 0x38da, 0x4b32, 0xac, 0xf3, 0xfa, 0x97, 0x34, 0xad, 0x80, 0x0e)
WGC_IID(Session, 0x814e42a9, 0xf70f, 0x4ad7, 0x93, 0x9b, 0xfd, 0xdc, 0xc6, 0xeb, 0x88, 0x0d)
WGC_IID(Session2, 0x2c39ae40, 0x7d2e, 0x5044, 0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e)
WGC_IID(Session3, 0xf2cdd966, 0x22ae, 0x5ea1, 0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe)
WGC_IID(Device, 0xa37624ab, 0x8d5f, 0x4650, 0x9d, 0x3e, 0x9e, 0xaa, 0xee, 0x5c, 0xca, 0xc0)
WGC_IID(Surface, 0x0bf4a146, 0x13c1, 0x4694, 0xbe, 0xe3, 0x7a, 0xbf, 0x5f, 0x1e, 0x3f, 0x3f)
WGC_IID(Closable, 0x30d5a829, 0x7fa4, 0x4026, 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e)
#undef WGC_IID
} // namespace wgc
#endif

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#endif


namespace {

const uint16_t kMaxCanvas = 8192;
const uint32_t kBackground = 0xFF202020; // canvas areas not covered by any window

template <class T>
void SafeRelease(T*& p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

// Interop interfaces from windows.graphics.capture.interop.h and
// windows.graphics.directx.direct3d11.interop.h, declared here because not
// every SDK / MinGW version ships those headers.
const IID kIID_GraphicsCaptureItemInterop =
	{ 0x3628e81b, 0x3cac, 0x4c60, { 0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56 } };
const IID kIID_Direct3DDxgiInterfaceAccess =
	{ 0xa9b3d012, 0x3df2, 0x4ee3, { 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1 } };

struct ICaptureItemInterop : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void** result) = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void** result) = 0;
};
struct IDxgiInterfaceAccess : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

// WinRT entry points, loaded at run time (no import of combase / d3d11
// WinRT functions, so the server still starts where they are missing).
struct RoApi {
	typedef HRESULT(WINAPI* PFN_RoInitialize)(RO_INIT_TYPE);
	typedef HRESULT(WINAPI* PFN_RoGetActivationFactory)(HSTRING, REFIID, void**);
	typedef HRESULT(WINAPI* PFN_WindowsCreateString)(PCWSTR, UINT32, HSTRING*);
	typedef HRESULT(WINAPI* PFN_WindowsDeleteString)(HSTRING);
	typedef HRESULT(WINAPI* PFN_CreateDevice)(IDXGIDevice*, IInspectable**);
	PFN_RoInitialize roInitialize = nullptr;
	PFN_RoGetActivationFactory getFactory = nullptr;
	PFN_WindowsCreateString createString = nullptr;
	PFN_WindowsDeleteString deleteString = nullptr;
	PFN_CreateDevice createDevice = nullptr; // CreateDirect3D11DeviceFromDXGIDevice
	bool ok = false;
};

template <class F>
F Proc(HMODULE m, const char* name)
{
	return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

const RoApi& Api()
{
	static RoApi api;
	static std::once_flag once;
	std::call_once(once, [] {
		HMODULE combase = LoadLibraryExW(L"combase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		HMODULE d3d = LoadLibraryExW(L"d3d11.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		api.roInitialize = Proc<RoApi::PFN_RoInitialize>(combase, "RoInitialize");
		api.getFactory = Proc<RoApi::PFN_RoGetActivationFactory>(combase, "RoGetActivationFactory");
		api.createString = Proc<RoApi::PFN_WindowsCreateString>(combase, "WindowsCreateString");
		api.deleteString = Proc<RoApi::PFN_WindowsDeleteString>(combase, "WindowsDeleteString");
		api.createDevice = Proc<RoApi::PFN_CreateDevice>(d3d, "CreateDirect3D11DeviceFromDXGIDevice");
		api.ok = api.roInitialize && api.getFactory && api.createString && api.deleteString && api.createDevice;
	});
	return api;
}

HRESULT GetFactory(const wchar_t* className, REFIID iid, void** out)
{
	const RoApi& api = Api();
	HSTRING name = nullptr;
	HRESULT hr = api.createString(className, static_cast<UINT32>(wcslen(className)), &name);
	if (SUCCEEDED(hr)) {
		hr = api.getFactory(name, iid, out);
		api.deleteString(name);
	}
	return hr;
}

std::string Hr(const char* what, HRESULT hr)
{
	char buf[96];
	snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
	return buf;
}

RECT VisibleBounds(HWND hwnd)
{
	RECT r;
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
		GetWindowRect(hwnd, &r);
	return r;
}

bool IsCloaked(HWND hwnd)
{
	DWORD cloaked = 0;
	return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

// Copy `src` (BGRA, srcW x srcH, placed at srcRect on screen) into the
// canvas where it overlaps `part`.
void Blit(std::vector<uint8_t>& canvas, const RECT& canvasRect, const uint8_t* src, int srcW, int srcH,
          const RECT& srcRect, const RECT& part)
{
	RECT r;
	if (!IntersectRect(&r, &part, &srcRect) || !IntersectRect(&r, &r, &canvasRect))
		return;
	const int canvasW = canvasRect.right - canvasRect.left;
	for (int y = r.top; y < r.bottom; ++y) {
		const int sy = y - srcRect.top;
		if (sy < 0 || sy >= srcH)
			continue;
		const int sx = r.left - srcRect.left;
		const int n = (std::min)(static_cast<int>(r.right - r.left), srcW - sx);
		if (sx < 0 || n <= 0)
			continue;
		uint32_t* d = reinterpret_cast<uint32_t*>(canvas.data()) +
			size_t(y - canvasRect.top) * canvasW + (r.left - canvasRect.left);
		const uint32_t* s = reinterpret_cast<const uint32_t*>(src) + size_t(sy) * srcW + sx;
		for (int x = 0; x < n; ++x)
			d[x] = s[x] | 0xFF000000; // opaque; the alpha of captures varies
	}
}

} // namespace

// ===========================================================================
// Windows.Graphics.Capture for one window
// ===========================================================================

struct vncWindowCapture::Wgc {
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	wgc::Device* rtDevice = nullptr;
	wgc::Item* item = nullptr;
	wgc::FramePool* pool = nullptr;
	wgc::Session* session = nullptr;
	wgc::SizeInt32 poolSize = {};
	ID3D11Texture2D* staging = nullptr;
	int stagingW = 0, stagingH = 0;

	~Wgc()
	{
		wgc::Closable* c = nullptr;
		if (session && SUCCEEDED(session->QueryInterface(wgc::Iid<wgc::Closable>(),
				reinterpret_cast<void**>(&c)))) {
			c->Close();
			c->Release();
		}
		if (pool && SUCCEEDED(pool->QueryInterface(wgc::Iid<wgc::Closable>(),
				reinterpret_cast<void**>(&c)))) {
			c->Close();
			c->Release();
		}
		SafeRelease(session);
		SafeRelease(pool);
		SafeRelease(item);
		SafeRelease(rtDevice);
		SafeRelease(staging);
		SafeRelease(context);
		SafeRelease(device);
	}

	bool Init(HWND target, std::string& error)
	{
		const RoApi& api = Api();
		if (!api.ok) {
			error = "Windows.Graphics.Capture is not available (Windows 10 1903 or later needed)";
			return false;
		}
		api.roInitialize(RO_INIT_MULTITHREADED); // S_FALSE / RPC_E_CHANGED_MODE are fine

		HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context);
		if (FAILED(hr)) {
			error = Hr("D3D11CreateDevice", hr);
			return false;
		}
		IDXGIDevice* dxgi = nullptr;
		IInspectable* insp = nullptr;
		hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
		if (SUCCEEDED(hr))
			hr = api.createDevice(dxgi, &insp);
		SafeRelease(dxgi);
		if (SUCCEEDED(hr))
			hr = insp->QueryInterface(wgc::Iid<wgc::Device>(), reinterpret_cast<void**>(&rtDevice));
		SafeRelease(insp);
		if (FAILED(hr)) {
			error = Hr("CreateDirect3D11DeviceFromDXGIDevice", hr);
			return false;
		}

		ICaptureItemInterop* interop = nullptr;
		hr = GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", kIID_GraphicsCaptureItemInterop,
			reinterpret_cast<void**>(&interop));
		if (SUCCEEDED(hr)) {
			hr = interop->CreateForWindow(target, wgc::Iid<wgc::Item>(), reinterpret_cast<void**>(&item));
			interop->Release();
		}
		if (FAILED(hr)) {
			error = Hr("GraphicsCaptureItem::CreateForWindow", hr);
			return false;
		}
		item->get_Size(&poolSize);

		wgc::FramePoolStatics2* statics = nullptr;
		hr = GetFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool",
			wgc::Iid<wgc::FramePoolStatics2>(), reinterpret_cast<void**>(&statics));
		if (SUCCEEDED(hr)) {
			// Free threaded: frames are polled with TryGetNextFrame, no
			// dispatcher or event handler needed.
			hr = statics->CreateFreeThreaded(rtDevice, wgc::kFormatBgra, 2, poolSize, &pool);
			statics->Release();
		}
		if (FAILED(hr)) {
			error = Hr("Direct3D11CaptureFramePool::CreateFreeThreaded", hr);
			return false;
		}
		hr = pool->CreateCaptureSession(item, &session);
		if (FAILED(hr)) {
			error = Hr("CreateCaptureSession", hr);
			return false;
		}
		// The pointer is drawn by the viewer; the yellow border can only be
		// turned off on Windows 11 (accepted on Windows 10, see the design).
		wgc::Session2* s2 = nullptr;
		if (SUCCEEDED(session->QueryInterface(wgc::Iid<wgc::Session2>(), reinterpret_cast<void**>(&s2)))) {
			s2->put_IsCursorCaptureEnabled(FALSE);
			s2->Release();
		}
#ifdef UVNC_HAVE_WGC_SESSION3
		wgc::Session3* s3 = nullptr;
		if (SUCCEEDED(session->QueryInterface(wgc::Iid<wgc::Session3>(), reinterpret_cast<void**>(&s3)))) {
			s3->put_IsBorderRequired(FALSE);
			s3->Release();
		}
#endif
		hr = session->StartCapture();
		if (FAILED(hr)) {
			error = Hr("StartCapture", hr);
			return false;
		}
		return true;
	}

	// 1: new picture in out, 0: nothing new, -1: error.
	int Grab(std::vector<uint8_t>& out, int& outW, int& outH)
	{
		wgc::Frame* frame = nullptr;
		if (FAILED(pool->TryGetNextFrame(&frame)))
			return -1;
		if (!frame)
			return 0;
		int result = -1;
		wgc::SizeInt32 cs = {};
		wgc::Surface* surface = nullptr;
		IDxgiInterfaceAccess* access = nullptr;
		ID3D11Texture2D* tex = nullptr;
		frame->get_ContentSize(&cs);
		if (SUCCEEDED(frame->get_Surface(&surface)) &&
		    SUCCEEDED(surface->QueryInterface(kIID_Direct3DDxgiInterfaceAccess, reinterpret_cast<void**>(&access))) &&
		    SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
			D3D11_TEXTURE2D_DESC td;
			tex->GetDesc(&td);
			const int w = (std::min)(cs.Width, static_cast<INT32>(td.Width));
			const int h = (std::min)(cs.Height, static_cast<INT32>(td.Height));
			if (w > 0 && h > 0 && EnsureStaging(w, h)) {
				D3D11_BOX box = { 0, 0, 0, static_cast<UINT>(w), static_cast<UINT>(h), 1 };
				context->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);
				D3D11_MAPPED_SUBRESOURCE map;
				if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
					out.resize(size_t(w) * h * 4);
					for (int y = 0; y < h; ++y)
						memcpy(&out[size_t(y) * w * 4], static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch,
							size_t(w) * 4);
					context->Unmap(staging, 0);
					outW = w;
					outH = h;
					result = 1;
				}
			}
		}
		SafeRelease(tex);
		SafeRelease(access);
		SafeRelease(surface);
		wgc::Closable* c = nullptr;
		if (SUCCEEDED(frame->QueryInterface(wgc::Iid<wgc::Closable>(), reinterpret_cast<void**>(&c)))) {
			c->Close();
			c->Release();
		}
		frame->Release();
		// The window was resized: frames of the new size need a new pool.
		if (cs.Width > 0 && cs.Height > 0 && (cs.Width != poolSize.Width || cs.Height != poolSize.Height)) {
			poolSize = cs;
			pool->Recreate(rtDevice, wgc::kFormatBgra, 2, cs);
		}
		return result;
	}

	bool EnsureStaging(int w, int h)
	{
		if (staging && stagingW == w && stagingH == h)
			return true;
		SafeRelease(staging);
		D3D11_TEXTURE2D_DESC sd = {};
		sd.Width = w;
		sd.Height = h;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateTexture2D(&sd, NULL, &staging)))
			return false;
		stagingW = w;
		stagingH = h;
		return true;
	}
};

// ===========================================================================

vncWindowCapture::vncWindowCapture() = default;

vncWindowCapture::~vncWindowCapture()
{
	Stop();
}

bool vncWindowCapture::UsesWgc() const
{
	return m_wgc != nullptr;
}

bool vncWindowCapture::Start(HWND target, std::string& error)
{
	Stop();
	if (!IsWindow(target)) {
		error = "the window does not exist any more";
		return false;
	}
	m_target = target;
	std::unique_ptr<Wgc> wgc(new Wgc);
	std::string wgcError;
	if (wgc->Init(target, wgcError)) {
		m_wgc = std::move(wgc);
	} else {
		// PrintWindow works everywhere, just slower; keep going.
		error = "using PrintWindow: " + wgcError;
	}
	return true;
}

void vncWindowCapture::Stop()
{
	m_wgc.reset();
	m_target = NULL;
	m_main.clear();
	m_mainWidth = m_mainHeight = 0;
	m_canvas.clear();
	m_parts.clear();
	m_canvasRect = RECT{};
}

bool vncWindowCapture::BelongsTo(HWND hwnd, HWND target)
{
	if (hwnd == target)
		return true;
	for (HWND o = GetWindow(hwnd, GW_OWNER); o; o = GetWindow(o, GW_OWNER))
		if (o == target)
			return true;
	DWORD pid = 0, targetPid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	GetWindowThreadProcessId(target, &targetPid);
	// Menus, drop-down lists and tooltips are unowned pop-ups of the same
	// program.
	return pid == targetPid && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) != 0;
}

namespace {
struct PopupSearch {
	HWND target;
	std::vector<HWND>* out;
};

BOOL CALLBACK PopupEnumProc(HWND hwnd, LPARAM lp)
{
	PopupSearch* c = reinterpret_cast<PopupSearch*>(lp);
	if (hwnd == c->target)
		return FALSE;
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsCloaked(hwnd))
		return TRUE;
	if (vncWindowCapture::BelongsTo(hwnd, c->target)) {
		RECT r = VisibleBounds(hwnd);
		if (r.right > r.left && r.bottom > r.top)
			c->out->push_back(hwnd);
	}
	return TRUE;
}
} // namespace

void vncWindowCapture::FindPopups(std::vector<HWND>& popups)
{
	// EnumWindows goes from the top of the Z order down; pop-ups of interest
	// are above the window, so the search stops at the window itself.
	PopupSearch ctx = { m_target, &popups };
	EnumWindows(&PopupEnumProc, reinterpret_cast<LPARAM>(&ctx));
}

bool vncWindowCapture::CaptureMainWithPrintWindow()
{
	// PrintWindow waits for the window to draw itself; a hung program would
	// block this thread (and stopping the session) for good.
	if (IsHungAppWindow(m_target))
		return !m_main.empty(); // keep showing the last picture
	RECT wr;
	if (!GetWindowRect(m_target, &wr))
		return false;
	const int w = wr.right - wr.left, h = wr.bottom - wr.top;
	if (w <= 0 || h <= 0 || w > kMaxCanvas || h > kMaxCanvas)
		return false;
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	void* bits = nullptr;
	HDC screen = GetDC(NULL);
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	ReleaseDC(NULL, screen);
	bool ok = false;
	if (bmp && bits) {
		HGDIOBJ old = SelectObject(dc, bmp);
		const UINT PW_RENDERFULLCONTENT_ = 0x00000002; // also DirectX / browser content
		ok = PrintWindow(m_target, dc, PW_RENDERFULLCONTENT_) != FALSE;
		GdiFlush();
		if (ok) {
			m_main.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + size_t(w) * h * 4);
			m_mainWidth = w;
			m_mainHeight = h;
		}
		SelectObject(dc, old);
	}
	if (bmp)
		DeleteObject(bmp);
	DeleteDC(dc);
	return ok;
}

bool vncWindowCapture::CaptureMain()
{
	if (m_wgc) {
		int w = 0, h = 0;
		const int r = m_wgc->Grab(m_main, w, h);
		if (r > 0) {
			m_mainWidth = w;
			m_mainHeight = h;
		} else if (r < 0) {
			m_wgc.reset(); // fall back for the rest of the session
		}
		if (m_wgc)
			return true;
	}
	return CaptureMainWithPrintWindow();
}

vncWindowCapture::Result vncWindowCapture::Capture()
{
	if (!m_target || !IsWindow(m_target))
		return Result::Closed;
	if (IsIconic(m_target))
		return Result::Minimized;

	const RECT frame = VisibleBounds(m_target);
	RECT wr;
	GetWindowRect(m_target, &wr);
	if (!CaptureMain())
		return Result::Error;

	// Where the captured picture lies on the screen: WGC and PrintWindow
	// may include the invisible resize borders (window rectangle) or not
	// (visible frame bounds).
	RECT picRect;
	if (m_mainWidth == wr.right - wr.left && m_mainHeight == wr.bottom - wr.top)
		picRect = wr;
	else
		picRect = RECT{ frame.left, frame.top, frame.left + m_mainWidth, frame.top + m_mainHeight };

	std::vector<HWND> popups;
	FindPopups(popups);

	m_parts.clear();
	m_parts.push_back(frame);
	RECT canvas = frame;
	for (HWND p : popups) {
		RECT r = VisibleBounds(p);
		m_parts.push_back(r);
		UnionRect(&canvas, &canvas, &r);
	}
	if (canvas.right - canvas.left > kMaxCanvas)
		canvas.right = canvas.left + kMaxCanvas;
	if (canvas.bottom - canvas.top > kMaxCanvas)
		canvas.bottom = canvas.top + kMaxCanvas;
	if (canvas.right <= canvas.left || canvas.bottom <= canvas.top)
		return Result::Error;

	m_canvasRect = canvas;
	const size_t pixels = size_t(Width()) * Height();
	m_canvas.resize(pixels * 4);
	std::fill(reinterpret_cast<uint32_t*>(m_canvas.data()), reinterpret_cast<uint32_t*>(m_canvas.data()) + pixels,
		kBackground);

	if (!m_main.empty())
		Blit(m_canvas, m_canvasRect, m_main.data(), m_mainWidth, m_mainHeight, picRect, frame);

	// Pop-ups straight from the screen, bottom to top so the topmost wins.
	if (!popups.empty()) {
		HDC screen = GetDC(NULL);
		HDC dc = CreateCompatibleDC(screen);
		for (size_t i = popups.size(); i-- > 0;) {
			const RECT& r = m_parts[i + 1];
			const int w = r.right - r.left, h = r.bottom - r.top;
			if (w <= 0 || h <= 0 || w > kMaxCanvas || h > kMaxCanvas)
				continue;
			BITMAPINFO bi = {};
			bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
			bi.bmiHeader.biWidth = w;
			bi.bmiHeader.biHeight = -h;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			void* bits = nullptr;
			HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
			if (!bmp || !bits) {
				if (bmp)
					DeleteObject(bmp);
				continue;
			}
			HGDIOBJ old = SelectObject(dc, bmp);
			// CAPTUREBLT includes layered windows (menus with shadows).
			BitBlt(dc, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
			GdiFlush();
			Blit(m_canvas, m_canvasRect, static_cast<const uint8_t*>(bits), w, h, r, r);
			SelectObject(dc, old);
			DeleteObject(bmp);
		}
		DeleteDC(dc);
		ReleaseDC(NULL, screen);
	}
	return Result::Ok;
}
