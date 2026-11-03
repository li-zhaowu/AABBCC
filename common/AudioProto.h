// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Remote audio (docs/design.md section 5): wire format of extension
// channel 3, and the viewer's jitter buffer. Platform independent; unit
// tested in tests/audio_proto.
//
//   Start   viewer -> server  start sending what the remote plays
//   Format  server -> viewer  a new stream: codec, rate, channels, config
//   Data    server -> viewer  one encoded frame
//   Stop    viewer -> server
//   Status  server -> viewer  audio ended or was refused, and why
//
// Codec: AAC-LC through the encoder and decoder that ship with Windows
// (Media Foundation), so no extra library is needed. The codec byte allows
// adding Opus later.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace audio {

enum Subtype : uint8_t {
	kStart  = 1,
	kFormat = 2,
	kData   = 3,
	kStop   = 4,
	kStatus = 5,
};

enum Codec : uint8_t {
	kCodecAac = 1, // AAC-LC, raw frames (no ADTS), 1024 samples each
};

enum StartFlags : uint8_t {
	kStartMuteRemote = 0x01, // silence the remote speakers while streaming
};

enum StatusCode : uint32_t {
	kStatusStopped     = 1,
	kStatusUnsupported = 2, // no playback device, no encoder, unusable format
	kStatusDeviceLost  = 3, // the playback device went away and did not come back
	kStatusError       = 4,
};

constexpr uint32_t kDefaultBitrateKbps = 128;
constexpr size_t kMaxConfig = 64;

struct Start {
	uint8_t codec = kCodecAac;
	uint8_t flags = kStartMuteRemote;
	uint32_t bitrateKbps = kDefaultBitrateKbps;
};

struct Format {
	uint32_t streamId = 0;
	uint8_t codec = kCodecAac;
	uint8_t channels = 2;
	uint32_t sampleRate = 48000;
	uint32_t bitrateKbps = kDefaultBitrateKbps;
	std::vector<uint8_t> config; // decoder configuration (MF_MT_USER_DATA for AAC)
};

struct DataHeader {
	uint32_t streamId = 0;
	uint32_t seq = 0;
	uint64_t timestampUs = 0;
};
constexpr size_t kDataHeaderSize = 16;

struct Status {
	uint32_t code = 0;
	std::string message;
};

std::vector<uint8_t> EncodeStart(const Start& s);
bool DecodeStart(const uint8_t* p, size_t len, Start& out);

std::vector<uint8_t> EncodeFormat(const Format& f);
bool DecodeFormat(const uint8_t* p, size_t len, Format& out);

std::vector<uint8_t> EncodeData(const DataHeader& h, const uint8_t* frame, size_t n);
bool DecodeDataHeader(const uint8_t* p, size_t len, DataHeader& out);

std::vector<uint8_t> EncodeStatus(const Status& s);
bool DecodeStatus(const uint8_t* p, size_t len, Status& out);

// Interleaved 16 bit PCM FIFO between the decoder (network side) and the
// sound card (render side), in frames (one sample per channel).
//
// - At the start and after running dry it waits until `target` frames are
//   buffered, then plays (no stutter from playing every packet as soon as
//   it arrives).
// - If more than `max` frames pile up (the network delivered a burst after
//   a stall), the oldest are dropped back down to `target`, so the delay
//   does not keep growing.
class JitterBuffer {
public:
	JitterBuffer(int channels, size_t targetFrames, size_t maxFrames);

	void Push(const int16_t* samples, size_t frames);
	// Fills `frames` frames; returns how many came from the buffer, the rest
	// is silence.
	size_t Pull(int16_t* out, size_t frames);
	void Clear();

	size_t Buffered() const { return m_samples.size() / m_channels; }
	bool Waiting() const { return m_waiting; }
	uint64_t Underruns() const { return m_underruns; }
	uint64_t DroppedFrames() const { return m_dropped; }

private:
	int m_channels;
	size_t m_target;
	size_t m_max;
	std::deque<int16_t> m_samples;
	bool m_waiting = true;
	uint64_t m_underruns = 0;
	uint64_t m_dropped = 0;
};

} // namespace audio
