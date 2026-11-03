// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Single window remote control (docs/design.md section 4): wire format of
// extension channel 2 and the lossless tile transport. Platform independent;
// unit tested in tests/window_proto.
//
//   ListReq  viewer -> server  please list the windows I can pick
//   List     server -> viewer  top level windows: handle, title, program
//   Select   viewer -> server  show only this window (0: whole desktop again)
//   State    server -> viewer  session started / window minimized / closed ...
//   Canvas   server -> viewer  picture size (window plus its pop-ups)
//   Frame    server -> viewer  changed 64x64 tiles, zstd compressed BGRA
//   Ack      viewer -> server  frame applied (flow control: the server
//                              keeps at most two frames in flight)
//   Restore  viewer -> server  restore the minimized window
//
// While a window session runs, the viewer's framebuffer is the canvas and it
// stops asking for normal framebuffer updates; pointer events use canvas
// coordinates and the server maps them to the screen.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace window {

enum Subtype : uint8_t {
	kListReq = 1,
	kList    = 2,
	kSelect  = 3,
	kState   = 4,
	kCanvas  = 5,
	kFrame   = 6,
	kAck     = 7,
	kRestore = 8,
};

enum StateCode : uint8_t {
	kStateActive      = 1, // frames follow
	kStateMinimized   = 2, // no frames until restored
	kStateClosed      = 3, // the window is gone; session ended
	kStateUnavailable = 4, // secure desktop (UAC, lock screen) or capture failed; session ended
	kStateEnded       = 5, // ended on request (Select 0)
	kStateRefused     = 6, // the window cannot be shown (invalid, not allowed)
};

// The session ends with these states; the viewer goes back to the desktop.
inline bool IsFinal(uint8_t s)
{
	return s == kStateClosed || s == kStateUnavailable || s == kStateEnded || s == kStateRefused;
}

constexpr uint32_t kMaxListEntries = 1024;
constexpr size_t kMaxText = 512;
constexpr int kTileSize = 64;
constexpr uint16_t kMaxCanvas = 8192;

struct Entry {
	uint64_t handle = 0;
	uint32_t pid = 0;
	bool minimized = false;
	std::string title;   // UTF-8
	std::string program; // UTF-8, executable name
};

struct State {
	uint32_t sessionId = 0;
	uint8_t state = 0;
	std::string message; // UTF-8
};

struct Canvas {
	uint32_t sessionId = 0;
	uint16_t width = 0;
	uint16_t height = 0;
};

struct Rect {
	uint16_t x = 0, y = 0, w = 0, h = 0;
	bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct FrameHeader {
	uint32_t sessionId = 0;
	uint32_t frameNo = 0;
	uint16_t canvasWidth = 0;  // frames for another canvas size are dropped
	uint16_t canvasHeight = 0;
	std::vector<Rect> tiles;
	uint32_t rawSize = 0;      // size of the tile pixels before compression
	size_t dataOffset = 0;     // where the compressed pixels start (decode only)
};

std::vector<uint8_t> EncodeList(const std::vector<Entry>& entries);
bool DecodeList(const uint8_t* p, size_t len, std::vector<Entry>& out);

std::vector<uint8_t> EncodeSelect(uint64_t handle);
bool DecodeSelect(const uint8_t* p, size_t len, uint64_t& handle);

std::vector<uint8_t> EncodeState(const State& s);
bool DecodeState(const uint8_t* p, size_t len, State& out);

std::vector<uint8_t> EncodeCanvas(const Canvas& c);
bool DecodeCanvas(const uint8_t* p, size_t len, Canvas& out);

// Frame = header, tile list, then the compressed pixels.
std::vector<uint8_t> EncodeFrame(const FrameHeader& h, const std::vector<uint8_t>& compressed);
bool DecodeFrameHeader(const uint8_t* p, size_t len, FrameHeader& out);

std::vector<uint8_t> EncodeAck(uint32_t sessionId, uint32_t frameNo);
bool DecodeAck(const uint8_t* p, size_t len, uint32_t& sessionId, uint32_t& frameNo);

// --- tiles ------------------------------------------------------------------
// Pictures are 32 bpp BGRA, rows `stride` bytes apart.

// Tiles (kTileSize grid, clipped at the edges) that differ between prev and
// cur. With prev == nullptr every tile is returned.
std::vector<Rect> ChangedTiles(const uint8_t* prev, const uint8_t* cur, int width, int height, int stride);

// The pixels of the tiles, row by row, tile after tile.
std::vector<uint8_t> PackTiles(const uint8_t* pic, int stride, const std::vector<Rect>& tiles);

// Write packed tile pixels into a picture. Returns false if the data does
// not match the tile list or a tile lies outside width x height.
bool UnpackTiles(const uint8_t* packed, size_t size, const std::vector<Rect>& tiles,
                 uint8_t* pic, int width, int height, int stride);

size_t PackedSize(const std::vector<Rect>& tiles);

// zstd. Decompress fails if the result is not exactly `expected` bytes.
std::vector<uint8_t> Compress(const std::vector<uint8_t>& raw, int level = 1);
bool Decompress(const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& out);

} // namespace window
