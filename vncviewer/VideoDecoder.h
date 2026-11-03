// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Video mode, viewer side decoder: H.264 / HEVC through the Media
// Foundation decoder that ships with Windows (synchronous MFT). Works on any
// GPU vendor; the viewer does not need an NVIDIA card.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

struct IMFTransform;
struct IMFMediaType;

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

	// codec: video::kCodecH264 or video::kCodecHevc.
	bool Open(uint8_t codec, int width, int height, int fps, std::string& error);
	void Close();
	bool IsOpen() const { return m_mft != nullptr; }

	// Feed one access unit (Annex B). Calls onPicture for every picture that
	// comes out. Returns false if the decoder rejected the data; the caller
	// should then ask for a keyframe.
	bool Decode(const uint8_t* data, size_t size, int64_t timestamp100ns,
	            const PictureFn& onPicture, std::string& error);

private:
	bool SelectOutputType(std::string& error);
	bool DrainOutput(const PictureFn& onPicture, std::string& error);

	IMFTransform* m_mft = nullptr;
	int m_width = 0;          // visible size from the stream configuration
	int m_height = 0;
	UINT32 m_frameWidth = 0;  // decoder output size (may be aligned up)
	UINT32 m_frameHeight = 0;
	LONG m_stride = 0;
};
