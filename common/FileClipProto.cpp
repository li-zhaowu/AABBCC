// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileClipProto.h"
#include "ExtChannel.h"

#include <cstring>

namespace fileclip {

using extmsg::GetU16;
using extmsg::GetU32;
using extmsg::GetU64;
using extmsg::PutU16;
using extmsg::PutU32;
using extmsg::PutU64;

namespace {

// Bounds-checked reader over a byte buffer.
struct Reader {
	const uint8_t* p;
	size_t len;
	size_t pos = 0;
	bool ok = true;

	Reader(const uint8_t* data, size_t n) : p(data), len(n) {}

	bool Need(size_t n)
	{
		if (!ok || len - pos < n)
			ok = false;
		return ok;
	}
	uint8_t U8()
	{
		if (!Need(1)) return 0;
		return p[pos++];
	}
	uint16_t U16()
	{
		if (!Need(2)) return 0;
		uint16_t v = GetU16(p + pos);
		pos += 2;
		return v;
	}
	uint32_t U32()
	{
		if (!Need(4)) return 0;
		uint32_t v = GetU32(p + pos);
		pos += 4;
		return v;
	}
	uint64_t U64()
	{
		if (!Need(8)) return 0;
		uint64_t v = GetU64(p + pos);
		pos += 8;
		return v;
	}
	std::string Str(size_t n)
	{
		if (!Need(n)) return std::string();
		std::string s(reinterpret_cast<const char*>(p + pos), n);
		pos += n;
		return s;
	}
};

bool IEquals(const std::string& a, const char* b)
{
	size_t n = std::strlen(b);
	if (a.size() != n)
		return false;
	for (size_t i = 0; i < n; ++i) {
		char c = a[i];
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
		if (c != b[i])
			return false;
	}
	return true;
}

bool IsReservedDeviceName(const std::string& component)
{
	// Windows ignores the extension and trailing spaces for device names:
	// "nul.txt" and "CON .log" both refer to the device.
	std::string base = component.substr(0, component.find('.'));
	while (!base.empty() && base.back() == ' ')
		base.pop_back();

	static const char* const kNames[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
	for (const char* n : kNames)
		if (IEquals(base, n))
			return true;

	if (base.size() == 4 && (IEquals(base.substr(0, 3), "COM") || IEquals(base.substr(0, 3), "LPT"))) {
		const char d = base[3];
		if (d >= '0' && d <= '9')
			return true;
	}
	// COM and LPT followed by superscript 1, 2 or 3 (UTF-8 C2 B9, C2 B2, C2 B3).
	if (base.size() == 5 && (IEquals(base.substr(0, 3), "COM") || IEquals(base.substr(0, 3), "LPT"))) {
		const unsigned char a = static_cast<unsigned char>(base[3]);
		const unsigned char b = static_cast<unsigned char>(base[4]);
		if (a == 0xC2 && (b == 0xB9 || b == 0xB2 || b == 0xB3))
			return true;
	}
	return false;
}

bool IsSafeComponent(const std::string& c)
{
	if (c.empty() || c == "." || c == "..")
		return false;
	for (unsigned char ch : c) {
		if (ch < 0x20 || ch == 0x7F)
			return false;
		switch (ch) {
		case '<': case '>': case ':': case '"': case '|': case '?': case '*':
		case '\\': case '/':
			return false;
		default:
			break;
		}
	}
	if (c.back() == '.' || c.back() == ' ')
		return false;
	return !IsReservedDeviceName(c);
}

} // namespace

size_t Utf16Length(const std::string& s)
{
	size_t units = 0;
	size_t i = 0;
	const size_t n = s.size();
	while (i < n) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp;
		size_t extra;
		if (c < 0x80) {
			cp = c;
			extra = 0;
		} else if ((c & 0xE0) == 0xC0) {
			cp = c & 0x1F;
			extra = 1;
		} else if ((c & 0xF0) == 0xE0) {
			cp = c & 0x0F;
			extra = 2;
		} else if ((c & 0xF8) == 0xF0) {
			cp = c & 0x07;
			extra = 3;
		} else {
			return SIZE_MAX;
		}
		if (n - i - 1 < extra)
			return SIZE_MAX;
		for (size_t k = 1; k <= extra; ++k) {
			const unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80)
				return SIZE_MAX;
			cp = (cp << 6) | (cc & 0x3F);
		}
		// Reject overlong forms, surrogates and values above U+10FFFF.
		if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
		    (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
		    (cp >= 0xD800 && cp <= 0xDFFF))
			return SIZE_MAX;
		if (cp == 0)
			return SIZE_MAX;
		units += cp >= 0x10000 ? 2 : 1;
		i += extra + 1;
	}
	return units;
}

bool IsSafeRelativePath(const std::string& path)
{
	if (path.empty() || path.size() > kMaxPathBytes)
		return false;
	const size_t u16 = Utf16Length(path);
	if (u16 == SIZE_MAX || u16 > kMaxPathChars)
		return false;
	size_t start = 0;
	for (;;) {
		const size_t slash = path.find('/', start);
		const std::string comp = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (!IsSafeComponent(comp))
			return false;
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return true;
}

size_t FilterUnsafeEntries(Offer& offer, std::vector<uint32_t>& idMap)
{
	idMap.clear();
	std::vector<Entry> kept;
	kept.reserve(offer.entries.size());
	for (size_t i = 0; i < offer.entries.size(); ++i) {
		if (IsSafeRelativePath(offer.entries[i].path)) {
			kept.push_back(std::move(offer.entries[i]));
			idMap.push_back(static_cast<uint32_t>(i));
		}
	}
	const size_t removed = offer.entries.size() - kept.size();
	offer.entries.swap(kept);
	return removed;
}

// ---------------------------------------------------------------------------

std::vector<uint8_t> EncodeOffer(const Offer& o)
{
	std::vector<uint8_t> v;
	PutU32(v, o.seq);
	PutU32(v, static_cast<uint32_t>(o.entries.size()));
	for (const Entry& e : o.entries) {
		PutU32(v, e.attributes & kAttrMask);
		PutU64(v, e.size);
		PutU64(v, e.mtime);
		const size_t n = e.path.size() < kMaxPathBytes ? e.path.size() : kMaxPathBytes;
		PutU16(v, static_cast<uint16_t>(n));
		v.insert(v.end(), e.path.begin(), e.path.begin() + n);
	}
	return v;
}

bool DecodeOffer(const uint8_t* p, size_t len, Offer& out)
{
	Reader r(p, len);
	out.seq = r.U32();
	const uint32_t count = r.U32();
	if (!r.ok || count > kMaxEntries)
		return false;
	// Every entry needs at least 22 bytes; reject absurd counts early.
	if (static_cast<uint64_t>(count) * 22 > len)
		return false;
	out.entries.clear();
	out.entries.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		Entry e;
		e.attributes = r.U32() & kAttrMask;
		e.size = r.U64();
		e.mtime = r.U64();
		const uint16_t n = r.U16();
		if (n > kMaxPathBytes)
			return false;
		e.path = r.Str(n);
		if (!r.ok)
			return false;
		if (e.IsDirectory())
			e.size = 0;
		out.entries.push_back(std::move(e));
	}
	return r.ok;
}

std::vector<uint8_t> EncodeRevoke(uint32_t seq)
{
	std::vector<uint8_t> v;
	PutU32(v, seq);
	return v;
}

bool DecodeRevoke(const uint8_t* p, size_t len, uint32_t& seq)
{
	Reader r(p, len);
	seq = r.U32();
	return r.ok;
}

std::vector<uint8_t> EncodeRead(const Read& rd)
{
	std::vector<uint8_t> v;
	PutU32(v, rd.seq);
	PutU32(v, rd.streamId);
	PutU32(v, rd.fileId);
	PutU64(v, rd.offset);
	PutU32(v, rd.length);
	return v;
}

bool DecodeRead(const uint8_t* p, size_t len, Read& out)
{
	Reader r(p, len);
	out.seq = r.U32();
	out.streamId = r.U32();
	out.fileId = r.U32();
	out.offset = r.U64();
	out.length = r.U32();
	return r.ok && out.length > 0 && out.length <= kMaxReadLength;
}

std::vector<uint8_t> EncodeData(const DataHeader& h, const uint8_t* bytes, size_t n)
{
	std::vector<uint8_t> v;
	v.reserve(kDataHeaderSize + n);
	PutU32(v, h.streamId);
	PutU64(v, h.offset);
	v.push_back(h.flags);
	if (n > 0)
		v.insert(v.end(), bytes, bytes + n);
	return v;
}

bool DecodeDataHeader(const uint8_t* p, size_t len, DataHeader& out)
{
	Reader r(p, len);
	out.streamId = r.U32();
	out.offset = r.U64();
	out.flags = r.U8();
	return r.ok;
}

std::vector<uint8_t> EncodeCancel(uint32_t streamId)
{
	std::vector<uint8_t> v;
	PutU32(v, streamId);
	return v;
}

bool DecodeCancel(const uint8_t* p, size_t len, uint32_t& streamId)
{
	Reader r(p, len);
	streamId = r.U32();
	return r.ok;
}

std::vector<uint8_t> EncodeError(const Error& e)
{
	std::vector<uint8_t> v;
	PutU32(v, e.streamId);
	PutU32(v, e.code);
	return v;
}

bool DecodeError(const uint8_t* p, size_t len, Error& out)
{
	Reader r(p, len);
	out.streamId = r.U32();
	out.code = r.U32();
	return r.ok;
}

} // namespace fileclip
