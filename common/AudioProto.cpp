// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "AudioProto.h"
#include "ExtChannel.h"

#include <algorithm>
#include <cstring>

namespace audio {

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
	v.push_back(s.flags);
	PutU16(v, 0);
	PutU32(v, s.bitrateKbps);
	return v;
}

bool DecodeStart(const uint8_t* p, size_t len, Start& out)
{
	if (len < 8)
		return false;
	out.codec = p[0];
	out.flags = p[1];
	out.bitrateKbps = GetU32(p + 4);
	return out.bitrateKbps > 0;
}

std::vector<uint8_t> EncodeFormat(const Format& f)
{
	std::vector<uint8_t> v;
	PutU32(v, f.streamId);
	v.push_back(f.codec);
	v.push_back(f.channels);
	PutU16(v, 0);
	PutU32(v, f.sampleRate);
	PutU32(v, f.bitrateKbps);
	const size_t n = (std::min)(f.config.size(), kMaxConfig);
	PutU16(v, static_cast<uint16_t>(n));
	v.insert(v.end(), f.config.begin(), f.config.begin() + n);
	return v;
}

bool DecodeFormat(const uint8_t* p, size_t len, Format& out)
{
	if (len < 18)
		return false;
	out.streamId = GetU32(p);
	out.codec = p[4];
	out.channels = p[5];
	out.sampleRate = GetU32(p + 8);
	out.bitrateKbps = GetU32(p + 12);
	const size_t n = GetU16(p + 16);
	if (n > kMaxConfig || len - 18 < n)
		return false;
	out.config.assign(p + 18, p + 18 + n);
	return out.channels >= 1 && out.channels <= 2 && out.sampleRate >= 8000 && out.sampleRate <= 192000;
}

std::vector<uint8_t> EncodeData(const DataHeader& h, const uint8_t* frame, size_t n)
{
	std::vector<uint8_t> v;
	v.reserve(kDataHeaderSize + n);
	PutU32(v, h.streamId);
	PutU32(v, h.seq);
	PutU64(v, h.timestampUs);
	if (n > 0)
		v.insert(v.end(), frame, frame + n);
	return v;
}

bool DecodeDataHeader(const uint8_t* p, size_t len, DataHeader& out)
{
	if (len < kDataHeaderSize)
		return false;
	out.streamId = GetU32(p);
	out.seq = GetU32(p + 4);
	out.timestampUs = GetU64(p + 8);
	return true;
}

std::vector<uint8_t> EncodeStatus(const Status& s)
{
	std::vector<uint8_t> v;
	PutU32(v, s.code);
	const size_t n = (std::min)(s.message.size(), kMaxStatusMessage);
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

// --- JitterBuffer ---------------------------------------------------------------

JitterBuffer::JitterBuffer(int channels, size_t targetFrames, size_t maxFrames)
	: m_channels(channels > 0 ? channels : 1), m_target(targetFrames),
	  m_max((std::max)(maxFrames, targetFrames))
{
}

void JitterBuffer::Push(const int16_t* samples, size_t frames)
{
	m_samples.insert(m_samples.end(), samples, samples + frames * m_channels);
	const size_t buffered = Buffered();
	if (buffered > m_max) {
		const size_t drop = buffered - m_target;
		m_samples.erase(m_samples.begin(), m_samples.begin() + drop * m_channels);
		m_dropped += drop;
	}
}

size_t JitterBuffer::Pull(int16_t* out, size_t frames)
{
	if (m_waiting && Buffered() >= m_target)
		m_waiting = false;
	size_t got = 0;
	if (!m_waiting) {
		got = (std::min)(frames, Buffered());
		const size_t n = got * m_channels;
		std::copy(m_samples.begin(), m_samples.begin() + n, out);
		m_samples.erase(m_samples.begin(), m_samples.begin() + n);
		if (got < frames) {
			// Ran dry: build up the target again before playing on.
			m_waiting = true;
			++m_underruns;
		}
	}
	std::fill(out + got * m_channels, out + frames * m_channels, int16_t(0));
	return got;
}

void JitterBuffer::Clear()
{
	m_samples.clear();
	m_waiting = true;
}

} // namespace audio
