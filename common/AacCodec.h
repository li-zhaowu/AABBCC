// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// AAC-LC encoder (server) and decoder (viewer) for remote audio, using the
// Media Foundation transforms that ship with Windows. PCM is interleaved
// 16 bit; frames are raw AAC (no ADTS), 1024 samples per channel.

#pragma once

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct IMFTransform;

namespace audio {

// MF_MT_USER_DATA for raw AAC-LC: HEAACWAVEINFO tail + AudioSpecificConfig.
std::vector<uint8_t> AacUserData(int sampleRate, int channels);

class AacEncoder {
public:
	AacEncoder() = default;
	~AacEncoder() { Close(); }
	AacEncoder(const AacEncoder&) = delete;
	AacEncoder& operator=(const AacEncoder&) = delete;

	// sampleRate 44100 or 48000, 1 or 2 channels. The bitrate is rounded to
	// what the encoder supports (96, 128, 160 or 192 kbps).
	bool Open(int sampleRate, int channels, uint32_t kbps, std::string& error);
	void Close();
	uint32_t BitrateKbps() const { return m_kbps; }
	// Decoder configuration to send to the viewer.
	const std::vector<uint8_t>& Config() const { return m_config; }

	// Encode PCM; complete AAC frames are appended to `frames`.
	bool Encode(const int16_t* pcm, size_t sampleFrames, std::vector<std::vector<uint8_t>>& frames,
	            std::string& error);

private:
	bool Drain(std::vector<std::vector<uint8_t>>& frames, std::string& error);
	IMFTransform* m_mft = nullptr;
	int m_channels = 0;
	uint32_t m_kbps = 0;
	std::vector<uint8_t> m_config;
};

class AacDecoder {
public:
	AacDecoder() = default;
	~AacDecoder() { Close(); }
	AacDecoder(const AacDecoder&) = delete;
	AacDecoder& operator=(const AacDecoder&) = delete;

	// config: MF_MT_USER_DATA from the encoder (empty: built for AAC-LC).
	bool Open(int sampleRate, int channels, const std::vector<uint8_t>& config, std::string& error);
	void Close();
	bool IsOpen() const { return m_mft != nullptr; }

	// Decode one frame; PCM is appended to `pcm`.
	bool Decode(const uint8_t* frame, size_t size, std::vector<int16_t>& pcm, std::string& error);

private:
	IMFTransform* m_mft = nullptr;
};

} // namespace audio

#endif // _WIN32
