// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "VideoProto.h"
#include "ExtChannel.h"

#include <algorithm>

namespace video {

using extmsg::GetU16;
using extmsg::GetU32;
using extmsg::GetU64;
using extmsg::PutU16;
using extmsg::PutU32;
using extmsg::PutU64;

constexpr size_t kMaxStatusMessage = 1024;

std::vector<uint8_t> EncodeStart(const Start& s)
{
	std::vector<uint8_t> v;
	v.push_back(s.codec);
	v.push_back(0);
	PutU16(v, s.maxFps);
	PutU32(v, s.maxBitrateKbps);
	PutU16(v, s.maxWidth);
	PutU16(v, s.maxHeight);
	return v;
}

bool DecodeStart(const uint8_t* p, size_t len, Start& out)
{
	if (len < 12)
		return false;
	out.codec = p[0];
	out.maxFps = GetU16(p + 2);
	out.maxBitrateKbps = GetU32(p + 4);
	out.maxWidth = GetU16(p + 8);
	out.maxHeight = GetU16(p + 10);
	return out.maxFps > 0 && out.maxBitrateKbps > 0;
}

std::vector<uint8_t> EncodeConfig(const Config& c)
{
	std::vector<uint8_t> v;
	PutU32(v, c.streamId);
	v.push_back(c.codec);
	v.push_back(0);
	PutU16(v, c.width);
	PutU16(v, c.height);
	PutU16(v, static_cast<uint16_t>(c.x));
	PutU16(v, static_cast<uint16_t>(c.y));
	PutU16(v, c.fps);
	PutU32(v, c.bitrateKbps);
	return v;
}

bool DecodeConfig(const uint8_t* p, size_t len, Config& out)
{
	if (len < 20)
		return false;
	out.streamId = GetU32(p);
	out.codec = p[4];
	out.width = GetU16(p + 6);
	out.height = GetU16(p + 8);
	out.x = static_cast<int16_t>(GetU16(p + 10));
	out.y = static_cast<int16_t>(GetU16(p + 12));
	out.fps = GetU16(p + 14);
	out.bitrateKbps = GetU32(p + 16);
	return out.width > 0 && out.height > 0;
}

std::vector<uint8_t> EncodeFrame(const FrameHeader& h, const uint8_t* data, size_t n)
{
	std::vector<uint8_t> v;
	v.reserve(kFrameHeaderSize + n);
	PutU32(v, h.streamId);
	PutU32(v, h.frameNo);
	PutU64(v, h.captureTimeUs);
	v.push_back(h.flags);
	if (n > 0)
		v.insert(v.end(), data, data + n);
	return v;
}

bool DecodeFrameHeader(const uint8_t* p, size_t len, FrameHeader& out)
{
	if (len < kFrameHeaderSize)
		return false;
	out.streamId = GetU32(p);
	out.frameNo = GetU32(p + 4);
	out.captureTimeUs = GetU64(p + 8);
	out.flags = p[16];
	return true;
}

std::vector<uint8_t> EncodeFeedback(const Feedback& f)
{
	std::vector<uint8_t> v;
	PutU32(v, f.streamId);
	PutU32(v, f.lastFrameNo);
	PutU32(v, f.decodeUs);
	v.push_back(f.flags);
	return v;
}

bool DecodeFeedback(const uint8_t* p, size_t len, Feedback& out)
{
	if (len < 13)
		return false;
	out.streamId = GetU32(p);
	out.lastFrameNo = GetU32(p + 4);
	out.decodeUs = GetU32(p + 8);
	out.flags = p[12];
	return true;
}

std::vector<uint8_t> EncodeStatus(const Status& s)
{
	std::vector<uint8_t> v;
	PutU32(v, s.code);
	const size_t n = std::min(s.message.size(), kMaxStatusMessage);
	PutU16(v, static_cast<uint16_t>(n));
	v.insert(v.end(), s.message.begin(), s.message.begin() + n);
	return v;
}

bool DecodeStatus(const uint8_t* p, size_t len, Status& out)
{
	if (len < 6)
		return false;
	out.code = GetU32(p);
	const size_t n = GetU16(p + 4);
	if (n > kMaxStatusMessage || len - 6 < n)
		return false;
	out.message.assign(reinterpret_cast<const char*>(p + 6), n);
	return true;
}

bool ClipToFramebuffer(const Config& c, int fbWidth, int fbHeight, ClipResult& out)
{
	const int left = std::max<int>(c.x, 0);
	const int top = std::max<int>(c.y, 0);
	const int right = std::min<int>(c.x + c.width, fbWidth);
	const int bottom = std::min<int>(c.y + c.height, fbHeight);
	if (right <= left || bottom <= top)
		return false;
	out.dstX = left;
	out.dstY = top;
	out.srcX = left - c.x;
	out.srcY = top - c.y;
	out.width = right - left;
	out.height = bottom - top;
	return true;
}

} // namespace video
