// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Video mode, viewer side decoder: H.264 / HEVC / AV1 through the Media
// Foundation decoder that ships with Windows (synchronous MFT). When the
// decoder accepts a D3D11 device it decodes on the GPU (any vendor);
// otherwise, or if that fails, it decodes in software.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

struct IMFTransform;
struct IMFMediaType;
struct IMFDXGIDeviceManager;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

class VideoDecoder {
public:
	// One decoded picture in NV12: Y plane, then interleaved UV at half
	// resolution. Valid only during the callback.
	struct Picture {
		const uint8_t* y = nullptr;
		const uint8_t* uv = nullptr;
		int pitch = 0;
		int width = 0;
		int height = 0;
	};
	using PictureFn = std::function<void(const Picture&)>;

	VideoDecoder() = default;
	~VideoDecoder() { Close(); }

	VideoDecoder(const VideoDecoder&) = delete;
	VideoDecoder& operator=(const VideoDecoder&) = delete;

	// codec: video::kCodecH264, kCodecHevc or kCodecAv1. With allowHardware the
	// GPU is used if the decoder supports it.
	bool Open(uint8_t codec, int width, int height, int fps, bool allowHardware, std::string& error);
	void Close();
	bool IsOpen() const { return m_mft != nullptr; }
	bool IsHardware() const { return m_hardware; }

	// Feed one access unit (Annex B). Calls onPicture for every picture that
	// comes out. Returns false if the decoder rejected the data; the caller
	// should then ask for a keyframe.
	bool Decode(const uint8_t* data, size_t size, int64_t timestamp100ns,
	            const PictureFn& onPicture, std::string& error);

	// True if this machine can decode the codec on the GPU. Runs a short
	// probe on a helper thread the first time; cached afterwards.
	static bool HardwareAvailable(uint8_t codec);

private:
	bool SetupD3D(std::string& error);
	bool SelectOutputType(std::string& error);
	bool DrainOutput(const PictureFn& onPicture, std::string& error);
	bool DeliverGpuPicture(ID3D11Texture2D* tex, UINT subresource, const PictureFn& onPicture);

	IMFTransform* m_mft = nullptr;
	int m_width = 0;          // visible size from the stream configuration
	int m_height = 0;
	UINT32 m_frameWidth = 0;  // decoder output size (may be aligned up)
	UINT32 m_frameHeight = 0;
	LONG m_stride = 0;

	// Hardware decoding.
	bool m_hardware = false;
	ID3D11Device* m_device = nullptr;
	ID3D11DeviceContext* m_context = nullptr;
	IMFDXGIDeviceManager* m_dxgiManager = nullptr;
	UINT m_resetToken = 0;
	ID3D11Texture2D* m_staging = nullptr; // CPU readable copy of a picture
	bool m_gpuVerified = false;           // first GPU picture looked sane
	UINT m_stagingWidth = 0;
	UINT m_stagingHeight = 0;
};
