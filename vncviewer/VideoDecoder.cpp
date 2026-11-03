// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "VideoDecoder.h"
#include "common/VideoProto.h"

#include <d3d11.h>
#include <d3d10.h> // ID3D10Multithread
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <map>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#endif

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

bool StartMediaFoundation()
{
	static std::once_flag once;
	static bool ok = false;
	std::call_once(once, [] { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
	return ok;
}

} // namespace

bool VideoDecoder::SetupD3D(std::string& error)
{
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
		levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, NULL, &m_context);
	if (FAILED(hr)) {
		error = Hr("D3D11CreateDevice (video)", hr);
		return false;
	}
	// The decoder uses the device from its own threads.
	ID3D10Multithread* mt = nullptr;
	if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) {
		mt->SetMultithreadProtected(TRUE);
		mt->Release();
	}
	hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_dxgiManager);
	if (SUCCEEDED(hr))
		hr = m_dxgiManager->ResetDevice(m_device, m_resetToken);
	if (FAILED(hr)) {
		error = Hr("MFCreateDXGIDeviceManager", hr);
		return false;
	}
	return true;
}

bool VideoDecoder::Open(uint8_t codec, int width, int height, int fps, bool allowHardware, std::string& error)
{
	Close();
	// The reader thread calls us; give it a COM apartment once. It is never
	// uninitialised: the decoder may be destroyed on another thread.
	static thread_local bool comReady = false;
	if (!comReady) {
		const HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		comReady = SUCCEEDED(co) || co == RPC_E_CHANGED_MODE;
	}
	if (!StartMediaFoundation()) {
		error = "MFStartup failed (Media Foundation missing, e.g. Windows N without the Media Feature Pack)";
		return false;
	}
	m_width = width;
	m_height = height;

	// A synchronous (software) decoder: simple to drive and always present.
	const GUID subtype = codec == video::kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
	MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, subtype };
	IMFActivate** activates = nullptr;
	UINT32 count = 0;
	HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
		&in, NULL, &activates, &count);
	if (FAILED(hr) || count == 0) {
		CoTaskMemFree(activates);
		error = codec == video::kCodecHevc
			? "no HEVC decoder installed (install \"HEVC Video Extensions\" from the Microsoft Store)"
			: "no H.264 decoder found";
		return false;
	}
	hr = activates[0]->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(&m_mft));
	for (UINT32 i = 0; i < count; ++i)
		activates[i]->Release();
	CoTaskMemFree(activates);
	if (FAILED(hr)) {
		error = Hr("ActivateObject (video decoder)", hr);
		return false;
	}

	IMFAttributes* attrs = nullptr;
	UINT32 d3d11Aware = 0;
	if (SUCCEEDED(m_mft->GetAttributes(&attrs))) {
		attrs->SetUINT32(MF_LOW_LATENCY, TRUE); // output each picture at once
		d3d11Aware = MFGetAttributeUINT32(attrs, MF_SA_D3D11_AWARE, 0);
		attrs->Release();
	}

	// GPU decoding: hand the decoder a D3D11 device (before the media types).
	if (allowHardware && d3d11Aware) {
		std::string d3dError;
		if (SetupD3D(d3dError) &&
		    SUCCEEDED(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiManager)))) {
			m_hardware = true;
		} else {
			if (m_dxgiManager)
				m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
			SafeRelease(m_dxgiManager);
			SafeRelease(m_context);
			SafeRelease(m_device);
		}
	}

	IMFMediaType* inType = nullptr;
	MFCreateMediaType(&inType);
	inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	inType->SetGUID(MF_MT_SUBTYPE, subtype);
	MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
	MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, fps > 0 ? fps : 60, 1);
	inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	hr = m_mft->SetInputType(0, inType, 0);
	inType->Release();
	if (FAILED(hr)) {
		error = Hr("SetInputType", hr);
		Close();
		return false;
	}
	if (!SelectOutputType(error)) {
		Close();
		return false;
	}
	m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	return true;
}

void VideoDecoder::Close()
{
	if (m_mft) {
		m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
	}
	SafeRelease(m_mft);
	SafeRelease(m_staging);
	m_stagingWidth = m_stagingHeight = 0;
	SafeRelease(m_dxgiManager);
	SafeRelease(m_context);
	SafeRelease(m_device);
	m_hardware = false;
	m_gpuVerified = false;
}

bool VideoDecoder::DeliverGpuPicture(ID3D11Texture2D* tex, UINT subresource, const PictureFn& onPicture)
{
	D3D11_TEXTURE2D_DESC d;
	tex->GetDesc(&d);
	if (d.Format != DXGI_FORMAT_NV12)
		return false;
	if (!m_staging || m_stagingWidth != d.Width || m_stagingHeight != d.Height) {
		SafeRelease(m_staging);
		D3D11_TEXTURE2D_DESC sd = {};
		sd.Width = d.Width;
		sd.Height = d.Height;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.Format = DXGI_FORMAT_NV12;
		sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(m_device->CreateTexture2D(&sd, NULL, &m_staging)))
			return false;
		m_stagingWidth = d.Width;
		m_stagingHeight = d.Height;
	}
	// The decoder's textures are slices of one array; copy ours out.
	m_context->CopySubresourceRegion(m_staging, 0, 0, 0, 0, tex, subresource, NULL);
	D3D11_MAPPED_SUBRESOURCE map;
	if (FAILED(m_context->Map(m_staging, 0, D3D11_MAP_READ, 0, &map)))
		return false;
	Picture pic;
	pic.y = static_cast<const uint8_t*>(map.pData);
	pic.uv = pic.y + static_cast<size_t>(map.RowPitch) * d.Height;
	pic.pitch = static_cast<int>(map.RowPitch);
	pic.width = (std::min)(m_width, static_cast<int>(d.Width));
	pic.height = (std::min)(m_height, static_cast<int>(d.Height));

	// The copy above cannot report errors. A broken read-back (seen with
	// some drivers and emulators) gives an all-zero picture, which a real
	// stream cannot produce: limited range video has Y >= 16 and U, V
	// around 128 even for black. Check the first picture.
	if (!m_gpuVerified) {
		bool allZero = true;
		for (int i = 0; i < 16 && allZero; ++i) {
			const int x = (pic.width - 1) * (i % 4) / 3, yy = (pic.height - 1) * (i / 4) / 3;
			if (pic.y[size_t(yy) * pic.pitch + x] != 0 || pic.uv[size_t(yy / 2) * pic.pitch + (x & ~1)] != 0)
				allZero = false;
		}
		if (allZero) {
			m_context->Unmap(m_staging, 0);
			return false;
		}
		m_gpuVerified = true;
	}
	onPicture(pic);
	m_context->Unmap(m_staging, 0);
	return true;
}

bool VideoDecoder::HardwareAvailable(uint8_t codec)
{
	static std::mutex mutex;
	static std::map<uint8_t, bool> cache;
	std::lock_guard<std::mutex> lk(mutex);
	auto it = cache.find(codec);
	if (it != cache.end())
		return it->second;
	// On a helper thread: Open() puts the calling thread into the COM
	// multithreaded apartment, which the UI thread must not be.
	bool hw = false;
	std::thread probe([&] {
		VideoDecoder d;
		std::string error;
		hw = d.Open(codec, 1920, 1080, 60, true, error) && d.IsHardware();
	});
	probe.join();
	cache[codec] = hw;
	return hw;
}

bool VideoDecoder::SelectOutputType(std::string& error)
{
	for (DWORD i = 0;; ++i) {
		IMFMediaType* t = nullptr;
		HRESULT hr = m_mft->GetOutputAvailableType(0, i, &t);
		if (FAILED(hr)) {
			error = "the decoder offers no NV12 output";
			return false;
		}
		GUID sub;
		if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
			hr = m_mft->SetOutputType(0, t, 0);
			if (SUCCEEDED(hr)) {
				MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &m_frameWidth, &m_frameHeight);
				UINT32 stride = 0;
				m_stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))
					? static_cast<LONG>(stride) : static_cast<LONG>(m_frameWidth);
			}
			t->Release();
			if (FAILED(hr)) {
				error = Hr("SetOutputType", hr);
				return false;
			}
			return true;
		}
		t->Release();
	}
}

bool VideoDecoder::Decode(const uint8_t* data, size_t size, int64_t timestamp100ns,
                          const PictureFn& onPicture, std::string& error)
{
	if (!m_mft) {
		error = "decoder not open";
		return false;
	}
	IMFMediaBuffer* buf = nullptr;
	HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf);
	if (FAILED(hr)) {
		error = Hr("MFCreateMemoryBuffer", hr);
		return false;
	}
	BYTE* p = nullptr;
	buf->Lock(&p, NULL, NULL);
	memcpy(p, data, size);
	buf->Unlock();
	buf->SetCurrentLength(static_cast<DWORD>(size));
	IMFSample* sample = nullptr;
	MFCreateSample(&sample);
	sample->AddBuffer(buf);
	sample->SetSampleTime(timestamp100ns);
	buf->Release();

	hr = m_mft->ProcessInput(0, sample, 0);
	if (hr == MF_E_NOTACCEPTING) {
		// Take the pending pictures out first, then retry.
		if (!DrainOutput(onPicture, error)) {
			sample->Release();
			return false;
		}
		hr = m_mft->ProcessInput(0, sample, 0);
	}
	sample->Release();
	if (FAILED(hr)) {
		error = Hr("ProcessInput", hr);
		return false;
	}
	return DrainOutput(onPicture, error);
}

bool VideoDecoder::DrainOutput(const PictureFn& onPicture, std::string& error)
{
	for (;;) {
		MFT_OUTPUT_STREAM_INFO si = {};
		m_mft->GetOutputStreamInfo(0, &si);
		IMFSample* outSample = nullptr;
		const bool weAllocate = !(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
		if (weAllocate) {
			IMFMediaBuffer* ob = nullptr;
			if (FAILED(MFCreateMemoryBuffer(si.cbSize, &ob))) {
				error = "MFCreateMemoryBuffer (output) failed";
				return false;
			}
			MFCreateSample(&outSample);
			outSample->AddBuffer(ob);
			ob->Release();
		}
		MFT_OUTPUT_DATA_BUFFER out = {};
		out.pSample = outSample;
		DWORD status = 0;
		HRESULT hr = m_mft->ProcessOutput(0, 1, &out, &status);
		if (out.pEvents)
			out.pEvents->Release();

		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
			SafeRelease(outSample);
			return true;
		}
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			// The decoder found the real picture size in the SPS.
			SafeRelease(outSample);
			if (!SelectOutputType(error))
				return false;
			continue;
		}
		if (FAILED(hr)) {
			SafeRelease(outSample);
			error = Hr("ProcessOutput", hr);
			return false;
		}

		IMFSample* result = out.pSample;
		IMFMediaBuffer* mb = nullptr;
		bool delivered = false;
		// GPU decoding: the picture is a D3D11 texture.
		if (m_hardware && result && SUCCEEDED(result->GetBufferByIndex(0, &mb))) {
			IMFDXGIBuffer* dxgi = nullptr;
			if (SUCCEEDED(mb->QueryInterface(__uuidof(IMFDXGIBuffer), reinterpret_cast<void**>(&dxgi)))) {
				ID3D11Texture2D* tex = nullptr;
				UINT sub = 0;
				if (SUCCEEDED(dxgi->GetResource(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) &&
				    SUCCEEDED(dxgi->GetSubresourceIndex(&sub))) {
					delivered = DeliverGpuPicture(tex, sub, onPicture);
					if (!delivered) {
						tex->Release();
						dxgi->Release();
						mb->Release();
						if (result != outSample)
							SafeRelease(result);
						SafeRelease(outSample);
						error = "could not read the GPU decoded picture";
						return false;
					}
				}
				SafeRelease(tex);
				dxgi->Release();
			}
			SafeRelease(mb);
		}
		if (!delivered && result && SUCCEEDED(result->ConvertToContiguousBuffer(&mb))) {
			BYTE* base = nullptr;
			LONG pitch = 0;
			IMF2DBuffer* b2 = nullptr;
			bool locked2d = false, locked = false;
			if (SUCCEEDED(mb->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&b2))))
				locked2d = locked = SUCCEEDED(b2->Lock2D(&base, &pitch));
			if (!locked) {
				DWORD maxLen = 0, curLen = 0;
				locked = SUCCEEDED(mb->Lock(&base, &maxLen, &curLen));
				pitch = m_stride;
			}
			if (locked && base && pitch > 0) {
				Picture pic;
				pic.y = base;
				// NV12: the UV plane follows the (possibly aligned) Y plane.
				pic.uv = base + static_cast<size_t>(pitch) * m_frameHeight;
				pic.pitch = pitch;
				pic.width = (std::min)(m_width, static_cast<int>(m_frameWidth));
				pic.height = (std::min)(m_height, static_cast<int>(m_frameHeight));
				onPicture(pic);
			}
			if (locked2d)
				b2->Unlock2D();
			else if (locked)
				mb->Unlock();
			SafeRelease(b2);
			mb->Release();
		}
		if (result != outSample)
			SafeRelease(result);
		SafeRelease(outSample);
	}
}
