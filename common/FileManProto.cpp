// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileManProto.h"
#include "ExtChannel.h"

#include <cstring>

namespace fileman {

using extmsg::GetU16;
using extmsg::GetU32;
using extmsg::GetU64;
using extmsg::PutU16;
using extmsg::PutU32;
using extmsg::PutU64;

namespace {

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
	uint8_t U8() { return Need(1) ? p[pos++] : 0; }
	uint32_t U32()
	{
		if (!Need(4))
			return 0;
		const uint32_t v = GetU32(p + pos);
		pos += 4;
		return v;
	}
	uint64_t U64()
	{
		if (!Need(8))
			return 0;
		const uint64_t v = GetU64(p + pos);
		pos += 8;
		return v;
	}
	// u16 length + bytes, at most `max` bytes.
	std::string Str(size_t max)
	{
		if (!Need(2))
			return std::string();
		const size_t n = GetU16(p + pos);
		pos += 2;
		if (n > max) {
			ok = false;
			return std::string();
		}
		if (!Need(n))
			return std::string();
		std::string s(reinterpret_cast<const char*>(p + pos), n);
		pos += n;
		return s;
	}
	bool End() const { return ok; }
};

void PutStr(std::vector<uint8_t>& v, const std::string& s, size_t max)
{
	const size_t n = s.size() < max ? s.size() : max;
	PutU16(v, static_cast<uint16_t>(n));
	v.insert(v.end(), s.begin(), s.begin() + n);
}

bool ValidUtf8(const std::string& s)
{
	return Utf16Length(s) != SIZE_MAX;
}

bool IEquals(const std::string& a, const char* b)
{
	const size_t n = std::strlen(b);
	if (a.size() != n)
		return false;
	for (size_t i = 0; i < n; ++i) {
		char x = a[i];
		if (x >= 'a' && x <= 'z')
			x = static_cast<char>(x - 'a' + 'A');
		if (x != b[i])
			return false;
	}
	return true;
}

bool IsReservedName(const std::string& component)
{
	// The device name counts even with an extension ("CON.txt").
	std::string base = component.substr(0, component.find('.'));
	while (!base.empty() && base.back() == ' ')
		base.pop_back();
	static const char* const names[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
	for (const char* n : names)
		if (IEquals(base, n))
			return true;
	if (base.size() == 4 && (IEquals(base.substr(0, 3), "COM") || IEquals(base.substr(0, 3), "LPT")) &&
	    base[3] >= '1' && base[3] <= '9')
		return true;
	return false;
}

// One path component or a file name.
bool IsValidComponent(const std::string& c)
{
	if (c.empty() || c == "." || c == "..")
		return false;
	for (unsigned char ch : c) {
		if (ch < 32 || std::strchr("<>:\"/\\|?*", ch))
			return false;
	}
	if (c.back() == '.' || c.back() == ' ')
		return false;
	if (IsReservedName(c))
		return false;
	return Utf16Length(c) <= 255;
}

bool IsDriveLetter(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

std::vector<std::string> Split(const std::string& s, char sep)
{
	std::vector<std::string> out;
	size_t start = 0;
	for (;;) {
		const size_t i = s.find(sep, start);
		out.push_back(s.substr(start, i == std::string::npos ? std::string::npos : i - start));
		if (i == std::string::npos)
			break;
		start = i + 1;
	}
	return out;
}

} // namespace

const char* StatusName(uint32_t status)
{
	switch (status) {
	case kOk: return "OK";
	case kErrNotFound: return "not found";
	case kErrAccess: return "access denied";
	case kErrExists: return "already exists";
	case kErrBadPath: return "invalid path";
	case kErrIo: return "read/write error";
	case kErrBusy: return "too many open files";
	case kErrBadHandle: return "invalid handle";
	case kErrDiskFull: return "disk full";
	case kErrUnsupported: return "not supported";
	case kErrBadRequest: return "bad request";
	case kErrDisconnected: return "disconnected";
	case kErrCancelled: return "cancelled";
	case kErrTimeout: return "no answer from the server";
	default: return "error";
	}
}

size_t Utf16Length(const std::string& s)
{
	size_t units = 0;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		size_t n;
		uint32_t cp;
		if (c < 0x80) {
			n = 1;
			cp = c;
		} else if ((c & 0xE0) == 0xC0) {
			n = 2;
			cp = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			n = 3;
			cp = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			n = 4;
			cp = c & 0x07;
		} else {
			return SIZE_MAX;
		}
		if (i + n > s.size())
			return SIZE_MAX;
		for (size_t k = 1; k < n; ++k) {
			const unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80)
				return SIZE_MAX;
			cp = (cp << 6) | (cc & 0x3F);
		}
		// Overlong forms, surrogates, out of range.
		if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
		    (cp >= 0xD800 && cp <= 0xDFFF))
			return SIZE_MAX;
		units += cp >= 0x10000 ? 2 : 1;
		i += n;
	}
	return units;
}

// --- requests -------------------------------------------------------------

std::vector<uint8_t> EncodePathRequest(uint32_t reqId, const std::string& path)
{
	std::vector<uint8_t> v;
	PutU32(v, reqId);
	PutStr(v, path, kMaxPathBytes);
	return v;
}

bool DecodePathRequest(const uint8_t* p, size_t len, uint32_t& reqId, std::string& path)
{
	Reader r(p, len);
	reqId = r.U32();
	path = r.Str(kMaxPathBytes);
	return r.End();
}

std::vector<uint8_t> EncodeRootsRequest(uint32_t reqId)
{
	std::vector<uint8_t> v;
	PutU32(v, reqId);
	return v;
}

bool DecodeRootsRequest(const uint8_t* p, size_t len, uint32_t& reqId)
{
	Reader r(p, len);
	reqId = r.U32();
	return r.End();
}

std::vector<uint8_t> EncodeRename(const RenameRequest& q)
{
	std::vector<uint8_t> v;
	PutU32(v, q.reqId);
	PutStr(v, q.from, kMaxPathBytes);
	PutStr(v, q.to, kMaxPathBytes);
	return v;
}

bool DecodeRename(const uint8_t* p, size_t len, RenameRequest& out)
{
	Reader r(p, len);
	out.reqId = r.U32();
	out.from = r.Str(kMaxPathBytes);
	out.to = r.Str(kMaxPathBytes);
	return r.End();
}

std::vector<uint8_t> EncodeRead(const ReadRequest& q)
{
	std::vector<uint8_t> v;
	PutU32(v, q.reqId);
	PutU32(v, q.handle);
	PutU64(v, q.offset);
	PutU32(v, q.length);
	return v;
}

bool DecodeRead(const uint8_t* p, size_t len, ReadRequest& out)
{
	Reader r(p, len);
	out.reqId = r.U32();
	out.handle = r.U32();
	out.offset = r.U64();
	out.length = r.U32();
	return r.End() && out.length > 0 && out.length <= kMaxReadLength;
}

std::vector<uint8_t> EncodeHandleRequest(uint32_t reqId, uint32_t handle)
{
	std::vector<uint8_t> v;
	PutU32(v, reqId);
	PutU32(v, handle);
	return v;
}

bool DecodeHandleRequest(const uint8_t* p, size_t len, uint32_t& reqId, uint32_t& handle)
{
	Reader r(p, len);
	reqId = r.U32();
	handle = r.U32();
	return r.End();
}

std::vector<uint8_t> EncodeOpenWrite(const OpenWriteRequest& q)
{
	std::vector<uint8_t> v;
	PutU32(v, q.reqId);
	PutStr(v, q.path, kMaxPathBytes);
	PutU64(v, q.size);
	v.push_back(q.flags);
	PutU64(v, q.mtime);
	return v;
}

bool DecodeOpenWrite(const uint8_t* p, size_t len, OpenWriteRequest& out)
{
	Reader r(p, len);
	out.reqId = r.U32();
	out.path = r.Str(kMaxPathBytes);
	out.size = r.U64();
	out.flags = r.U8();
	if (!r.End())
		return false;
	out.mtime = r.pos < len ? r.U64() : 0; // older viewers: no time
	return r.End();
}

std::vector<uint8_t> EncodeWrite(const WriteHeader& h, const uint8_t* bytes, size_t n)
{
	std::vector<uint8_t> v;
	v.reserve(kWriteHeaderSize + n);
	PutU32(v, h.handle);
	PutU64(v, h.offset);
	if (n)
		v.insert(v.end(), bytes, bytes + n);
	return v;
}

bool DecodeWriteHeader(const uint8_t* p, size_t len, WriteHeader& out)
{
	if (len < kWriteHeaderSize || len - kWriteHeaderSize > kDataPieceBytes)
		return false;
	out.handle = GetU32(p);
	out.offset = GetU64(p + 4);
	return true;
}

std::vector<uint8_t> EncodeCloseWrite(const CloseWriteRequest& q)
{
	std::vector<uint8_t> v;
	PutU32(v, q.reqId);
	PutU32(v, q.handle);
	PutU64(v, q.mtime);
	v.push_back(q.commit ? 1 : 0);
	return v;
}

bool DecodeCloseWrite(const uint8_t* p, size_t len, CloseWriteRequest& out)
{
	Reader r(p, len);
	out.reqId = r.U32();
	out.handle = r.U32();
	out.mtime = r.U64();
	out.commit = r.U8() != 0;
	return r.End();
}

// --- replies ---------------------------------------------------------------

std::vector<uint8_t> EncodeReply(const ReplyHeader& h, const std::vector<uint8_t>& body)
{
	std::vector<uint8_t> v;
	PutU32(v, h.reqId);
	PutU32(v, h.status);
	v.push_back(h.flags);
	PutStr(v, h.message, kMaxMessageText);
	v.insert(v.end(), body.begin(), body.end());
	return v;
}

bool DecodeReplyHeader(const uint8_t* p, size_t len, ReplyHeader& out, size_t& bodyOffset)
{
	Reader r(p, len);
	out.reqId = r.U32();
	out.status = r.U32();
	out.flags = r.U8();
	out.message = r.Str(kMaxMessageText);
	bodyOffset = r.pos;
	return r.End();
}

std::vector<uint8_t> EncodeRoots(const std::vector<Root>& roots)
{
	std::vector<uint8_t> v;
	PutU32(v, static_cast<uint32_t>(roots.size()));
	for (const Root& root : roots) {
		PutStr(v, root.path, kMaxPathBytes);
		PutStr(v, root.label, kMaxMessageText);
		v.push_back(root.type);
		PutU64(v, root.totalBytes);
		PutU64(v, root.freeBytes);
	}
	return v;
}

bool DecodeRoots(const uint8_t* p, size_t len, std::vector<Root>& out)
{
	Reader r(p, len);
	const uint32_t n = r.U32();
	if (!r.End() || n > 1000)
		return false;
	out.clear();
	for (uint32_t i = 0; i < n && r.End(); ++i) {
		Root root;
		root.path = r.Str(kMaxPathBytes);
		root.label = r.Str(kMaxMessageText);
		root.type = r.U8();
		root.totalBytes = r.U64();
		root.freeBytes = r.U64();
		out.push_back(std::move(root));
	}
	return r.End();
}

std::vector<uint8_t> EncodeEntries(const std::vector<Entry>& entries)
{
	std::vector<uint8_t> v;
	PutU32(v, static_cast<uint32_t>(entries.size()));
	for (const Entry& e : entries) {
		PutStr(v, e.name, kMaxPathBytes);
		PutU32(v, e.attributes & kAttrMask);
		PutU64(v, e.size);
		PutU64(v, e.mtime);
	}
	return v;
}

bool DecodeEntries(const uint8_t* p, size_t len, std::vector<Entry>& out)
{
	Reader r(p, len);
	const uint32_t n = r.U32();
	if (!r.End() || n > kListPageEntries)
		return false;
	out.clear();
	out.reserve(n);
	for (uint32_t i = 0; i < n && r.End(); ++i) {
		Entry e;
		e.name = r.Str(kMaxPathBytes);
		e.attributes = r.U32() & kAttrMask;
		e.size = r.U64();
		e.mtime = r.U64();
		out.push_back(std::move(e));
	}
	return r.End();
}

std::vector<uint8_t> EncodeStat(const Entry& e)
{
	std::vector<uint8_t> v;
	PutU32(v, e.attributes & kAttrMask);
	PutU64(v, e.size);
	PutU64(v, e.mtime);
	return v;
}

bool DecodeStat(const uint8_t* p, size_t len, Entry& out)
{
	Reader r(p, len);
	out.attributes = r.U32() & kAttrMask;
	out.size = r.U64();
	out.mtime = r.U64();
	return r.End();
}

std::vector<uint8_t> EncodeOpenReadReply(const OpenReadReply& o)
{
	std::vector<uint8_t> v;
	PutU32(v, o.handle);
	PutU64(v, o.size);
	PutU64(v, o.mtime);
	return v;
}

bool DecodeOpenReadReply(const uint8_t* p, size_t len, OpenReadReply& out)
{
	Reader r(p, len);
	out.handle = r.U32();
	out.size = r.U64();
	out.mtime = r.U64();
	return r.End();
}

std::vector<uint8_t> EncodeOpenWriteReply(const OpenWriteReply& o)
{
	std::vector<uint8_t> v;
	PutU32(v, o.handle);
	PutU64(v, o.offset);
	return v;
}

bool DecodeOpenWriteReply(const uint8_t* p, size_t len, OpenWriteReply& out)
{
	Reader r(p, len);
	out.handle = r.U32();
	if (!r.End())
		return false;
	out.offset = r.pos < len ? r.U64() : 0; // older servers: no offset
	return r.End();
}

std::string ResumeTempName(const std::string& targetPath, uint64_t size, uint64_t mtime)
{
	// FNV-1a over the ASCII-lowercased path, the size and the time.
	uint64_t h = 1469598103934665603ULL;
	auto mix = [&](uint8_t b) {
		h ^= b;
		h *= 1099511628211ULL;
	};
	for (char c : targetPath)
		mix(static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
	for (int i = 0; i < 8; ++i)
		mix(static_cast<uint8_t>(size >> (8 * i)));
	for (int i = 0; i < 8; ++i)
		mix(static_cast<uint8_t>(mtime >> (8 * i)));
	static const char hex[] = "0123456789abcdef";
	std::string name = "~uvnc-";
	for (int i = 15; i >= 0; --i)
		name += hex[(h >> (4 * i)) & 15];
	return name + ".part";
}

std::vector<uint8_t> EncodeData(const DataHeader& h, const uint8_t* bytes, size_t n)
{
	std::vector<uint8_t> v;
	v.reserve(kDataHeaderSize + n);
	PutU32(v, h.handle);
	PutU64(v, h.offset);
	v.push_back(h.flags);
	if (n)
		v.insert(v.end(), bytes, bytes + n);
	return v;
}

bool DecodeDataHeader(const uint8_t* p, size_t len, DataHeader& out)
{
	if (len < kDataHeaderSize || len - kDataHeaderSize > kDataPieceBytes)
		return false;
	out.handle = GetU32(p);
	out.offset = GetU64(p + 4);
	out.flags = p[12];
	return true;
}

std::vector<uint8_t> EncodeWriteAck(const WriteAck& a)
{
	std::vector<uint8_t> v;
	PutU32(v, a.handle);
	PutU32(v, a.status);
	PutU64(v, a.written);
	return v;
}

bool DecodeWriteAck(const uint8_t* p, size_t len, WriteAck& out)
{
	Reader r(p, len);
	out.handle = r.U32();
	out.status = r.U32();
	out.written = r.U64();
	return r.End();
}

// --- paths -----------------------------------------------------------------

bool IsAcceptablePath(const std::string& s)
{
	if (s.empty() || s.size() > kMaxPathBytes || !ValidUtf8(s) || Utf16Length(s) > kMaxPathChars)
		return false;
	std::vector<std::string> parts;
	if (s.size() >= 3 && IsDriveLetter(s[0]) && s[1] == ':' && s[2] == '\\') {
		if (s.size() == 3)
			return true; // "C:\"
		parts = Split(s.substr(3), '\\');
	} else if (s.size() > 2 && s[0] == '\\' && s[1] == '\\') {
		// UNC: \\server\share[\...]; "\\?\" and "\\.\" are refused by the
		// component check ("?" is forbidden, "." is not a valid component).
		parts = Split(s.substr(2), '\\');
		if (parts.size() < 2)
			return false;
	} else {
		return false;
	}
	for (const std::string& c : parts)
		if (!IsValidComponent(c))
			return false;
	return true;
}

bool IsRootPath(const std::string& s)
{
	if (s.size() == 3 && IsDriveLetter(s[0]) && s[1] == ':' && s[2] == '\\')
		return true;
	if (s.size() > 2 && s[0] == '\\' && s[1] == '\\') {
		const auto parts = Split(s.substr(2), '\\');
		return parts.size() == 2 || (parts.size() == 3 && parts[2].empty());
	}
	return false;
}

bool IsValidName(const std::string& s)
{
	return ValidUtf8(s) && IsValidComponent(s);
}

std::string JoinPath(const std::string& dir, const std::string& name)
{
	if (dir.empty())
		return name;
	if (dir.back() == '\\')
		return dir + name;
	return dir + "\\" + name;
}

std::string ParentPath(const std::string& path)
{
	if (IsRootPath(path))
		return std::string();
	std::string p = path;
	if (p.size() > 1 && p.back() == '\\')
		p.pop_back();
	const size_t i = p.rfind('\\');
	if (i == std::string::npos)
		return std::string();
	std::string parent = p.substr(0, i);
	if (parent.size() == 2 && parent[1] == ':')
		parent += '\\'; // "C:" -> "C:\"
	if (parent.size() <= 2)
		return std::string(); // "\\" of a UNC path
	return parent;
}

std::string FileNamePart(const std::string& path)
{
	if (IsRootPath(path))
		return path;
	std::string p = path;
	if (p.size() > 1 && p.back() == '\\')
		p.pop_back();
	const size_t i = p.rfind('\\');
	return i == std::string::npos ? p : p.substr(i + 1);
}

} // namespace fileman
