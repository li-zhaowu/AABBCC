// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// NVIDIA hardware encoder (NVENC) for video mode. Encodes BGRA D3D11
// textures to H.264 / HEVC / AV1 with a low latency configuration: no B frames,
// CBR, endless GOP with IDR frames only on request.
//
// The NVENC API header comes from the NVIDIA Video Codec SDK (MIT license),
// packaged by vcpkg as "ffnvcodec". The encoder DLL (nvEncodeAPI64.dll) is
// part of the NVIDIA driver and is loaded at run time
// (nvEncodeAPI.dll in 32 bit builds).

#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

#include <ffnvcodec/nvEncodeAPI.h>

class vncNvencEncoder {
public:
	vncNvencEncoder() = default;
	~vncNvencEncoder() { Close(); }

	vncNvencEncoder(const vncNvencEncoder&) = delete;
	vncNvencEncoder& operator=(const vncNvencEncoder&) = delete;

	// True if the NVIDIA driver provides an NVENC API at least as new as the
	// header we were built with. Cheap after the first call.
	static bool IsAvailable(std::string* why);

	// codec: video::kCodecH264, kCodecHevc or kCodecAv1. Fails (with a
	// message) if the GPU has no encoder for it.
	bool Open(ID3D11Device* device, int width, int height, int fps, uint32_t bitrateKbps,
	          uint8_t codec, std::string& error);
	void Close();

	// Encode the top-left Width() x Height() part of a BGRA texture of the
	// same device. The result is an Annex B byte stream; with forceIdr the
	// picture is an IDR frame preceded by SPS/PPS.
	bool Encode(ID3D11Texture2D* source, bool forceIdr, uint64_t timestamp,
	            std::vector<uint8_t>& out, bool& isKeyframe, std::string& error);

	bool SetBitrate(uint32_t kbps);

	bool IsOpen() const { return m_encoder != nullptr; }
	int Width() const { return m_width; }
	int Height() const { return m_height; }
	uint32_t BitrateKbps() const { return m_bitrateKbps; }

private:
	static bool LoadApi(std::string* why);

	void* m_encoder = nullptr;
	ID3D11Device* m_device = nullptr;
	ID3D11DeviceContext* m_context = nullptr;
	ID3D11Texture2D* m_input = nullptr;
	NV_ENC_REGISTERED_PTR m_registered = nullptr;
	NV_ENC_OUTPUT_PTR m_bitstream = nullptr;
	NV_ENC_INITIALIZE_PARAMS m_init = {};
	NV_ENC_CONFIG m_config = {};
	int m_width = 0;
	int m_height = 0;
	int m_fps = 0;
	uint32_t m_bitrateKbps = 0;
};
