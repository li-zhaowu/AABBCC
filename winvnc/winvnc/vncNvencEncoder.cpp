// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "vncNvencEncoder.h"
#include "common/VideoProto.h"

#include <cstring>
#include <mutex>

namespace {

typedef NVENCSTATUS(NVENCAPI* PFN_CreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS(NVENCAPI* PFN_GetMaxSupportedVersion)(uint32_t*);

NV_ENCODE_API_FUNCTION_LIST g_api = {};
bool g_apiOk = false;
std::string g_apiError;

std::string NvError(const char* what, NVENCSTATUS st)
{
	return std::string(what) + " failed (NVENC status " + std::to_string(static_cast<int>(st)) + ")";
}

template <class T>
void SafeRelease(T*& p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

} // namespace

bool vncNvencEncoder::LoadApi(std::string* why)
{
	static std::once_flag once;
	std::call_once(once, [] {
#ifdef _WIN64
		const wchar_t* dllName = L"nvEncodeAPI64.dll";
#else
		const wchar_t* dllName = L"nvEncodeAPI.dll";
#endif
		HMODULE dll = LoadLibraryExW(dllName, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!dll) {
			g_apiError = "the NVENC library of the NVIDIA driver was not found (no NVIDIA driver?)";
			return;
		}
		auto getMax = reinterpret_cast<PFN_GetMaxSupportedVersion>(reinterpret_cast<void*>(
			GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion")));
		auto create = reinterpret_cast<PFN_CreateInstance>(reinterpret_cast<void*>(
			GetProcAddress(dll, "NvEncodeAPICreateInstance")));
		if (!getMax || !create) {
			g_apiError = "the NVENC library has no NVENC entry points";
			return;
		}
		uint32_t driverVersion = 0;
		const uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
		if (getMax(&driverVersion) != NV_ENC_SUCCESS || driverVersion < needed) {
			g_apiError = "the NVIDIA driver is too old for NVENC API " +
				std::to_string(NVENCAPI_MAJOR_VERSION) + "." + std::to_string(NVENCAPI_MINOR_VERSION) +
				" (please update the driver)";
			return;
		}
		g_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
		if (create(&g_api) != NV_ENC_SUCCESS || !g_api.nvEncOpenEncodeSessionEx) {
			g_apiError = "NvEncodeAPICreateInstance failed";
			return;
		}
		g_apiOk = true;
	});
	if (!g_apiOk && why)
		*why = g_apiError;
	return g_apiOk;
}

bool vncNvencEncoder::IsAvailable(std::string* why)
{
	return LoadApi(why);
}

bool vncNvencEncoder::Open(ID3D11Device* device, int width, int height, int fps, uint32_t bitrateKbps,
                           uint8_t codec, std::string& error)
{
	Close();
	if (!LoadApi(&error))
		return false;

	// 4:2:0 needs even dimensions; drop a last odd row/column.
	m_width = width & ~1;
	m_height = height & ~1;
	m_fps = fps > 0 ? fps : video::kDefaultFps;
	m_bitrateKbps = bitrateKbps;
	const GUID codecGuid = codec == video::kCodecHevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
	if (codec != video::kCodecHevc && (m_width > 4096 || m_height > 4096)) {
		error = "H.264 supports at most 4096x4096";
		return false;
	}

	m_device = device;
	m_device->AddRef();
	m_device->GetImmediateContext(&m_context);

	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp = {};
	sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	sp.device = device;
	sp.apiVersion = NVENCAPI_VERSION;
	NVENCSTATUS st = g_api.nvEncOpenEncodeSessionEx(&sp, &m_encoder);
	if (st != NV_ENC_SUCCESS) {
		m_encoder = nullptr;
		error = NvError("nvEncOpenEncodeSessionEx", st);
		Close();
		return false;
	}

	// Start from the driver's low latency preset and adjust.
	NV_ENC_PRESET_CONFIG preset = {};
	preset.version = NV_ENC_PRESET_CONFIG_VER;
	preset.presetCfg.version = NV_ENC_CONFIG_VER;
	st = g_api.nvEncGetEncodePresetConfigEx(m_encoder, codecGuid, NV_ENC_PRESET_P4_GUID,
		NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
	if (st != NV_ENC_SUCCESS) {
		error = NvError("nvEncGetEncodePresetConfigEx", st);
		Close();
		return false;
	}
	m_config = preset.presetCfg;
	m_config.version = NV_ENC_CONFIG_VER;
	m_config.gopLength = NVENC_INFINITE_GOPLENGTH;
	m_config.frameIntervalP = 1; // no B frames
	m_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	m_config.rcParams.averageBitRate = m_bitrateKbps * 1000;
	m_config.rcParams.maxBitRate = m_bitrateKbps * 1000;
	// A VBV of about one frame keeps every frame close to the average size,
	// so a big change on screen does not cause a burst that queues up.
	m_config.rcParams.vbvBufferSize = m_bitrateKbps * 1000 / m_fps;
	m_config.rcParams.vbvInitialDelay = m_config.rcParams.vbvBufferSize;
	if (codec == video::kCodecHevc) {
		m_config.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
		m_config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
	} else {
		m_config.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
		m_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
	}

	m_init = {};
	m_init.version = NV_ENC_INITIALIZE_PARAMS_VER;
	m_init.encodeGUID = codecGuid;
	m_init.presetGUID = NV_ENC_PRESET_P4_GUID;
	m_init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
	m_init.encodeWidth = m_width;
	m_init.encodeHeight = m_height;
	m_init.darWidth = m_width;
	m_init.darHeight = m_height;
	m_init.maxEncodeWidth = m_width;
	m_init.maxEncodeHeight = m_height;
	m_init.frameRateNum = m_fps;
	m_init.frameRateDen = 1;
	m_init.enablePTD = 1;
	m_init.encodeConfig = &m_config;
	st = g_api.nvEncInitializeEncoder(m_encoder, &m_init);
	if (st != NV_ENC_SUCCESS) {
		error = NvError("nvEncInitializeEncoder", st);
		Close();
		return false;
	}

	// Input texture owned by us; the captured picture is copied into it.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = m_width;
	td.Height = m_height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = m_device->CreateTexture2D(&td, NULL, &m_input);
	if (FAILED(hr)) {
		error = "CreateTexture2D for the encoder input failed";
		Close();
		return false;
	}

	NV_ENC_REGISTER_RESOURCE reg = {};
	reg.version = NV_ENC_REGISTER_RESOURCE_VER;
	reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
	reg.width = m_width;
	reg.height = m_height;
	reg.resourceToRegister = m_input;
	reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB; // = DXGI B8G8R8A8; NVENC converts to YUV
	reg.bufferUsage = NV_ENC_INPUT_IMAGE;
	st = g_api.nvEncRegisterResource(m_encoder, &reg);
	if (st != NV_ENC_SUCCESS) {
		error = NvError("nvEncRegisterResource", st);
		Close();
		return false;
	}
	m_registered = reg.registeredResource;

	NV_ENC_CREATE_BITSTREAM_BUFFER bb = {};
	bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
	st = g_api.nvEncCreateBitstreamBuffer(m_encoder, &bb);
	if (st != NV_ENC_SUCCESS) {
		error = NvError("nvEncCreateBitstreamBuffer", st);
		Close();
		return false;
	}
	m_bitstream = bb.bitstreamBuffer;
	return true;
}

void vncNvencEncoder::Close()
{
	if (m_encoder) {
		// Flush (end of stream) before destroying.
		NV_ENC_PIC_PARAMS eos = {};
		eos.version = NV_ENC_PIC_PARAMS_VER;
		eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
		g_api.nvEncEncodePicture(m_encoder, &eos);
		if (m_bitstream)
			g_api.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstream);
		if (m_registered)
			g_api.nvEncUnregisterResource(m_encoder, m_registered);
		g_api.nvEncDestroyEncoder(m_encoder);
	}
	m_encoder = nullptr;
	m_bitstream = nullptr;
	m_registered = nullptr;
	SafeRelease(m_input);
	SafeRelease(m_context);
	SafeRelease(m_device);
}

bool vncNvencEncoder::Encode(ID3D11Texture2D* source, bool forceIdr, uint64_t timestamp,
                             std::vector<uint8_t>& out, bool& isKeyframe, std::string& error)
{
	out.clear();
	isKeyframe = false;
	if (!m_encoder) {
		error = "encoder not open";
		return false;
	}

	D3D11_BOX box = { 0, 0, 0, static_cast<UINT>(m_width), static_cast<UINT>(m_height), 1 };
	m_context->CopySubresourceRegion(m_input, 0, 0, 0, 0, source, 0, &box);

	NV_ENC_MAP_INPUT_RESOURCE map = {};
	map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	map.registeredResource = m_registered;
	NVENCSTATUS st = g_api.nvEncMapInputResource(m_encoder, &map);
	if (st != NV_ENC_SUCCESS) {
		error = NvError("nvEncMapInputResource", st);
		return false;
	}

	NV_ENC_PIC_PARAMS pic = {};
	pic.version = NV_ENC_PIC_PARAMS_VER;
	pic.inputBuffer = map.mappedResource;
	pic.bufferFmt = map.mappedBufferFmt;
	pic.inputWidth = m_width;
	pic.inputHeight = m_height;
	pic.outputBitstream = m_bitstream;
	pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	pic.inputTimeStamp = timestamp;
	if (forceIdr)
		pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
	st = g_api.nvEncEncodePicture(m_encoder, &pic);

	bool ok = st == NV_ENC_SUCCESS;
	if (ok) {
		NV_ENC_LOCK_BITSTREAM lock = {};
		lock.version = NV_ENC_LOCK_BITSTREAM_VER;
		lock.outputBitstream = m_bitstream;
		st = g_api.nvEncLockBitstream(m_encoder, &lock); // waits for the GPU
		if (st == NV_ENC_SUCCESS) {
			const uint8_t* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
			out.assign(p, p + lock.bitstreamSizeInBytes);
			isKeyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
			g_api.nvEncUnlockBitstream(m_encoder, m_bitstream);
		} else {
			ok = false;
			error = NvError("nvEncLockBitstream", st);
		}
	} else {
		error = NvError("nvEncEncodePicture", st);
	}
	g_api.nvEncUnmapInputResource(m_encoder, map.mappedResource);
	return ok;
}

bool vncNvencEncoder::SetBitrate(uint32_t kbps)
{
	if (!m_encoder || kbps == 0)
		return false;
	if (kbps == m_bitrateKbps)
		return true;
	m_config.rcParams.averageBitRate = kbps * 1000;
	m_config.rcParams.maxBitRate = kbps * 1000;
	m_config.rcParams.vbvBufferSize = kbps * 1000 / m_fps;
	m_config.rcParams.vbvInitialDelay = m_config.rcParams.vbvBufferSize;
	NV_ENC_RECONFIGURE_PARAMS rp = {};
	rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	rp.reInitEncodeParams = m_init;
	rp.reInitEncodeParams.encodeConfig = &m_config;
	if (g_api.nvEncReconfigureEncoder(m_encoder, &rp) != NV_ENC_SUCCESS)
		return false;
	m_bitrateKbps = kbps;
	return true;
}
