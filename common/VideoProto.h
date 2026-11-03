// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Wire format of the video channel (extension channel 4, docs/design.md
// section 3). Platform independent; unit tested in tests/video_proto.
//
//   Start     viewer -> server  switch this connection to video mode
//   Config    server -> viewer  a new stream: size, position, codec
//   Frame     server -> viewer  one encoded picture (Annex B byte stream)
//   Feedback  viewer -> server  progress and "please send a keyframe"
//   Stop      viewer -> server  back to the classic (office) mode
//   Status    server -> viewer  video mode ended or was refused, and why
//   Cursor    server -> viewer  the remote mouse pointer shape changed
//
// While video mode is active the viewer stops asking for framebuffer
// updates, so the server sends only video. The video picture covers one
// monitor; Config tells the viewer where that monitor sits in the
// framebuffer (it may be partly outside, the viewer clips).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace video {

enum Subtype : uint8_t {
	kStart    = 1,
	kConfig   = 2,
	kFrame    = 3,
	kFeedback = 4,
	kStop     = 5,
	kStatus   = 6,
	kCursor   = 7,
};

enum Codec : uint8_t {
	kCodecH264 = 1,
	kCodecHevc = 2,
};

enum FrameFlags : uint8_t {
	kFrameKey = 0x01, // IDR: decoding can start here
};

enum FeedbackFlags : uint8_t {
	kFeedbackKeyframe = 0x01, // decoder lost sync, send an IDR
};

enum StatusCode : uint32_t {
	kStatusStopped      = 1, // the viewer asked to stop
	kStatusUnsupported  = 2, // no usable encoder or capture on this server
	kStatusCaptureLost  = 3, // the screen could not be captured any more
	kStatusEncoderError = 4,
	kStatusScaling      = 5, // server side scaling is active
	kStatusBadRequest   = 6,
};

// Defaults agreed in the design (section 7).
constexpr uint16_t kDefaultFps         = 60;
constexpr uint32_t kDefaultBitrateKbps = 20000;

enum StartFlags : uint8_t {
	// Only remember the parameters, do not stream the monitor: used when a
	// single window is to be shown as video (the window session then feeds
	// the encoder).
	kStartParamsOnly = 0x01,
};

struct Start {
	uint8_t codec = kCodecH264;
	uint8_t flags = 0;
	uint16_t maxFps = kDefaultFps;
	uint32_t maxBitrateKbps = kDefaultBitrateKbps;
	uint16_t maxWidth = 0;   // 0 = no limit
	uint16_t maxHeight = 0;
};

struct Config {
	uint32_t streamId = 0;   // frames carry it; older streams are ignored
	uint8_t codec = kCodecH264;
	uint16_t width = 0;      // picture size
	uint16_t height = 0;
	int16_t x = 0;           // picture position in the framebuffer
	int16_t y = 0;
	uint16_t fps = 0;
	uint32_t bitrateKbps = 0;
};

struct FrameHeader {
	uint32_t streamId = 0;
	uint32_t frameNo = 0;
	uint64_t captureTimeUs = 0; // server clock, for latency measurements
	uint8_t flags = 0;
};
constexpr size_t kFrameHeaderSize = 17;

struct Feedback {
	uint32_t streamId = 0;
	uint32_t lastFrameNo = 0;
	uint32_t decodeUs = 0;
	uint8_t flags = 0;
};

enum CursorType : uint8_t {
	kCursorHidden     = 0, // no pointer (e.g. games): show none
	kCursorMonochrome = 1, // 1 bpp AND mask rows, then XOR mask rows (2 x height rows)
	kCursorColor      = 2, // 32 bpp BGRA with straight alpha
};
constexpr uint16_t kMaxCursorSize = 256;

struct Cursor {
	uint8_t type = kCursorHidden;
	uint16_t hotX = 0;
	uint16_t hotY = 0;
	uint16_t width = 0;
	uint16_t height = 0;     // visible height (monochrome data has 2x rows)
	uint32_t pitch = 0;      // bytes per row in data
	std::vector<uint8_t> data;
};

struct Status {
	uint32_t code = 0;
	std::string message; // UTF-8, for logs and the user
};

std::vector<uint8_t> EncodeStart(const Start& s);
bool DecodeStart(const uint8_t* p, size_t len, Start& out);

std::vector<uint8_t> EncodeConfig(const Config& c);
bool DecodeConfig(const uint8_t* p, size_t len, Config& out);

// Frame = header followed by the bitstream.
std::vector<uint8_t> EncodeFrame(const FrameHeader& h, const uint8_t* data, size_t n);
bool DecodeFrameHeader(const uint8_t* p, size_t len, FrameHeader& out);

std::vector<uint8_t> EncodeFeedback(const Feedback& f);
bool DecodeFeedback(const uint8_t* p, size_t len, Feedback& out);

std::vector<uint8_t> EncodeCursor(const Cursor& c);
bool DecodeCursor(const uint8_t* p, size_t len, Cursor& out);

std::vector<uint8_t> EncodeStatus(const Status& s);
bool DecodeStatus(const uint8_t* p, size_t len, Status& out);

// Intersection of the picture rectangle with the framebuffer, in picture
// coordinates (srcX/srcY) and framebuffer coordinates (dstX/dstY). Returns
// false if nothing is visible.
struct ClipResult {
	int srcX = 0, srcY = 0, dstX = 0, dstY = 0, width = 0, height = 0;
};
bool ClipToFramebuffer(const Config& c, int fbWidth, int fbHeight, ClipResult& out);

} // namespace video
