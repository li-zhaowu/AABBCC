// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "WindowProto.h"
#include "ExtChannel.h"

#include <algorithm>
#include <cstring>

#include <zstd.h> // vcpkg port "zstd" (already used by UltraVNC)

namespace window {

using extmsg::GetU16;
using extmsg::GetU32;
using extmsg::GetU64;
using extmsg::PutU16;
using extmsg::PutU32;
using extmsg::PutU64;

namespace {

void PutText(std::vector<uint8_t>& v, const std::string& s)
{
	const size_t n = std::min(s.size(), kMaxText);
	PutU16(v, static_cast<uint16_t>(n));
	v.insert(v.end(), s.begin(), s.begin() + n);
}

struct Reader {
	const uint8_t* p;
	size_t len;
	size_t pos = 0;
	bool ok = true;
	Reader(const uint8_t* d, size_t n) : p(d), len(n) {}
	bool Need(size_t n)
	{
		if (!ok || len - pos < n)
			ok = false;
		return ok;
	}
	uint8_t U8() { return Need(1) ? p[pos++] : 0; }
	uint16_t U16() { if (!Need(2)) return 0; uint16_t v = GetU16(p + pos); pos += 2; return v; }
	uint32_t U32() { if (!Need(4)) return 0; uint32_t v = GetU32(p + pos); pos += 4; return v; }
	uint64_t U64() { if (!Need(8)) return 0; uint64_t v = GetU64(p + pos); pos += 8; return v; }
	std::string Text()
	{
		const uint16_t n = U16();
		if (n > kMaxText || !Need(n)) {
			ok = false;
			return std::string();
		}
		std::string s(reinterpret_cast<const char*>(p + pos), n);
		pos += n;
		return s;
	}
};

} // namespace

std::vector<uint8_t> EncodeList(const std::vector<Entry>& entries)
{
	std::vector<uint8_t> v;
	const size_t n = std::min<size_t>(entries.size(), kMaxListEntries);
	PutU32(v, static_cast<uint32_t>(n));
	for (size_t i = 0; i < n; ++i) {
		const Entry& e = entries[i];
		PutU64(v, e.handle);
		PutU32(v, e.pid);
		v.push_back(e.minimized ? 1 : 0);
		PutText(v, e.title);
		PutText(v, e.program);
	}
	return v;
}

bool DecodeList(const uint8_t* p, size_t len, std::vector<Entry>& out)
{
	Reader r(p, len);
	const uint32_t n = r.U32();
	if (!r.ok || n > kMaxListEntries)
		return false;
	out.clear();
	for (uint32_t i = 0; i < n && r.ok; ++i) {
		Entry e;
		e.handle = r.U64();
		e.pid = r.U32();
		e.minimized = (r.U8() & 1) != 0;
		e.title = r.Text();
		e.program = r.Text();
		out.push_back(std::move(e));
	}
	return r.ok;
}

std::vector<uint8_t> EncodeSelect(uint64_t handle, uint8_t transport)
{
	std::vector<uint8_t> v;
	PutU64(v, handle);
	v.push_back(transport);
	return v;
}

bool DecodeSelect(const uint8_t* p, size_t len, uint64_t& handle, uint8_t& transport)
{
	Reader r(p, len);
	handle = r.U64();
	transport = len > 8 ? r.U8() : static_cast<uint8_t>(kTransportTiles);
	return r.ok && transport <= kTransportVideo;
}

std::vector<uint8_t> EncodeState(const State& s)
{
	std::vector<uint8_t> v;
	PutU32(v, s.sessionId);
	v.push_back(s.state);
	PutText(v, s.message);
	return v;
}

bool DecodeState(const uint8_t* p, size_t len, State& out)
{
	Reader r(p, len);
	out.sessionId = r.U32();
	out.state = r.U8();
	out.message = r.Text();
	return r.ok;
}

std::vector<uint8_t> EncodeCanvas(const Canvas& c)
{
	std::vector<uint8_t> v;
	PutU32(v, c.sessionId);
	PutU16(v, c.width);
	PutU16(v, c.height);
	return v;
}

bool DecodeCanvas(const uint8_t* p, size_t len, Canvas& out)
{
	Reader r(p, len);
	out.sessionId = r.U32();
	out.width = r.U16();
	out.height = r.U16();
	return r.ok && out.width > 0 && out.height > 0 && out.width <= kMaxCanvas && out.height <= kMaxCanvas;
}

std::vector<uint8_t> EncodeFrame(const FrameHeader& h, const std::vector<uint8_t>& compressed)
{
	std::vector<uint8_t> v;
	v.reserve(20 + h.tiles.size() * 8 + compressed.size());
	PutU32(v, h.sessionId);
	PutU32(v, h.frameNo);
	PutU16(v, h.canvasWidth);
	PutU16(v, h.canvasHeight);
	PutU32(v, static_cast<uint32_t>(h.tiles.size()));
	for (const Rect& t : h.tiles) {
		PutU16(v, t.x);
		PutU16(v, t.y);
		PutU16(v, t.w);
		PutU16(v, t.h);
	}
	PutU32(v, h.rawSize);
	v.insert(v.end(), compressed.begin(), compressed.end());
	return v;
}

bool DecodeFrameHeader(const uint8_t* p, size_t len, FrameHeader& out)
{
	Reader r(p, len);
	out.sessionId = r.U32();
	out.frameNo = r.U32();
	out.canvasWidth = r.U16();
	out.canvasHeight = r.U16();
	const uint32_t n = r.U32();
	// A canvas of at most 8192 x 8192 has at most 128 x 128 tiles.
	if (!r.ok || n > (kMaxCanvas / kTileSize) * (kMaxCanvas / kTileSize) || !r.Need(size_t(n) * 8))
		return false;
	out.tiles.resize(n);
	for (Rect& t : out.tiles) {
		t.x = r.U16();
		t.y = r.U16();
		t.w = r.U16();
		t.h = r.U16();
	}
	out.rawSize = r.U32();
	out.dataOffset = r.pos;
	return r.ok && out.rawSize == PackedSize(out.tiles);
}

std::vector<uint8_t> EncodeAck(uint32_t sessionId, uint32_t frameNo)
{
	std::vector<uint8_t> v;
	PutU32(v, sessionId);
	PutU32(v, frameNo);
	return v;
}

bool DecodeAck(const uint8_t* p, size_t len, uint32_t& sessionId, uint32_t& frameNo)
{
	Reader r(p, len);
	sessionId = r.U32();
	frameNo = r.U32();
	return r.ok;
}

// --- tiles ------------------------------------------------------------------

std::vector<Rect> ChangedTiles(const uint8_t* prev, const uint8_t* cur, int width, int height, int stride)
{
	std::vector<Rect> out;
	for (int ty = 0; ty < height; ty += kTileSize) {
		const int th = std::min(kTileSize, height - ty);
		for (int tx = 0; tx < width; tx += kTileSize) {
			const int tw = std::min(kTileSize, width - tx);
			bool changed = prev == nullptr;
			for (int y = ty; y < ty + th && !changed; ++y) {
				const size_t off = size_t(y) * stride + size_t(tx) * 4;
				changed = std::memcmp(prev + off, cur + off, size_t(tw) * 4) != 0;
			}
			if (changed) {
				Rect r;
				r.x = static_cast<uint16_t>(tx);
				r.y = static_cast<uint16_t>(ty);
				r.w = static_cast<uint16_t>(tw);
				r.h = static_cast<uint16_t>(th);
				out.push_back(r);
			}
		}
	}
	return out;
}

size_t PackedSize(const std::vector<Rect>& tiles)
{
	size_t n = 0;
	for (const Rect& t : tiles)
		n += size_t(t.w) * t.h * 4;
	return n;
}

std::vector<uint8_t> PackTiles(const uint8_t* pic, int stride, const std::vector<Rect>& tiles)
{
	std::vector<uint8_t> out(PackedSize(tiles));
	uint8_t* dst = out.data();
	for (const Rect& t : tiles) {
		for (int y = 0; y < t.h; ++y) {
			std::memcpy(dst, pic + size_t(t.y + y) * stride + size_t(t.x) * 4, size_t(t.w) * 4);
			dst += size_t(t.w) * 4;
		}
	}
	return out;
}

bool UnpackTiles(const uint8_t* packed, size_t size, const std::vector<Rect>& tiles,
                 uint8_t* pic, int width, int height, int stride)
{
	if (size != PackedSize(tiles))
		return false;
	for (const Rect& t : tiles) {
		if (t.w == 0 || t.h == 0 || t.x + t.w > width || t.y + t.h > height)
			return false;
	}
	const uint8_t* src = packed;
	for (const Rect& t : tiles) {
		for (int y = 0; y < t.h; ++y) {
			std::memcpy(pic + size_t(t.y + y) * stride + size_t(t.x) * 4, src, size_t(t.w) * 4);
			src += size_t(t.w) * 4;
		}
	}
	return true;
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& raw, int level)
{
	std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
	const size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
	if (ZSTD_isError(n))
		return std::vector<uint8_t>();
	out.resize(n);
	return out;
}

bool Decompress(const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& out)
{
	out.resize(expected);
	if (expected == 0)
		return size == 0 || !ZSTD_isError(ZSTD_decompress(nullptr, 0, data, size));
	const size_t n = ZSTD_decompress(out.data(), out.size(), data, size);
	return !ZSTD_isError(n) && n == expected;
}

} // namespace window
