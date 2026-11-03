// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Unit tests for common/FileClipProto. Build with tests/fileclip_proto/run.sh
// or with MSVC:
//   cl /std:c++17 /EHsc fileclip_proto_test.cpp ..\..\common\FileClipProto.cpp ..\..\common\ExtChannel.cpp

#include "../../common/FileClipProto.h"

#include <cstdio>
#include <cstring>

using namespace fileclip;

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

static void TestOfferRoundTrip()
{
	Offer o;
	o.seq = 42;
	Entry d;
	d.path = "folder";
	d.attributes = kAttrDirectory;
	d.size = 999; // must be dropped for directories
	Entry f;
	f.path = "folder/\xE4\xB8\xAD\xE6\x96\x87.txt"; // Chinese file name
	f.attributes = kAttrReadOnly | kAttrArchive | 0x80000; // unknown bit masked
	f.size = 5ull * 1024 * 1024 * 1024; // > 4 GB
	f.mtime = 0x01DB123456789ABCull;
	o.entries = { d, f };

	auto enc = EncodeOffer(o);
	Offer out;
	CHECK(DecodeOffer(enc.data(), enc.size(), out));
	CHECK(out.seq == 42);
	CHECK(out.entries.size() == 2);
	if (out.entries.size() == 2) {
		CHECK(out.entries[0].path == "folder" && out.entries[0].IsDirectory() && out.entries[0].size == 0);
		CHECK(out.entries[1].path == f.path);
		CHECK(out.entries[1].attributes == (kAttrReadOnly | kAttrArchive));
		CHECK(out.entries[1].size == f.size && out.entries[1].mtime == f.mtime);
	}

	// Every truncation must be rejected, never read out of bounds.
	for (size_t n = 0; n < enc.size(); ++n) {
		Offer t;
		CHECK(!DecodeOffer(enc.data(), n, t));
	}

	// Absurd entry count.
	std::vector<uint8_t> bad = { 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF };
	CHECK(!DecodeOffer(bad.data(), bad.size(), out));
	// Count within kMaxEntries but far more than the buffer can hold.
	bad = { 0, 0, 0, 1, 0, 0, 0x10, 0 };
	CHECK(!DecodeOffer(bad.data(), bad.size(), out));

	Offer empty;
	empty.seq = 7;
	enc = EncodeOffer(empty);
	CHECK(DecodeOffer(enc.data(), enc.size(), out) && out.seq == 7 && out.entries.empty());
}

static void TestSmallMessages()
{
	uint32_t v = 0;
	auto enc = EncodeRevoke(0xA1B2C3D4);
	CHECK(DecodeRevoke(enc.data(), enc.size(), v) && v == 0xA1B2C3D4);
	CHECK(!DecodeRevoke(enc.data(), 3, v));

	Read rd;
	rd.seq = 1;
	rd.streamId = 2;
	rd.fileId = 3;
	rd.offset = 0x123456789ull;
	rd.length = 1024 * 1024;
	enc = EncodeRead(rd);
	Read ro;
	CHECK(DecodeRead(enc.data(), enc.size(), ro));
	CHECK(ro.seq == 1 && ro.streamId == 2 && ro.fileId == 3 && ro.offset == rd.offset && ro.length == rd.length);
	CHECK(!DecodeRead(enc.data(), enc.size() - 1, ro));
	rd.length = 0;
	enc = EncodeRead(rd);
	CHECK(!DecodeRead(enc.data(), enc.size(), ro));
	rd.length = kMaxReadLength + 1;
	enc = EncodeRead(rd);
	CHECK(!DecodeRead(enc.data(), enc.size(), ro));

	DataHeader h;
	h.streamId = 9;
	h.offset = 1ull << 40;
	h.flags = kDataEof;
	const uint8_t bytes[] = { 1, 2, 3 };
	enc = EncodeData(h, bytes, sizeof(bytes));
	CHECK(enc.size() == kDataHeaderSize + 3);
	DataHeader ho;
	CHECK(DecodeDataHeader(enc.data(), enc.size(), ho));
	CHECK(ho.streamId == 9 && ho.offset == h.offset && ho.flags == kDataEof);
	CHECK(std::memcmp(enc.data() + kDataHeaderSize, bytes, 3) == 0);
	CHECK(!DecodeDataHeader(enc.data(), kDataHeaderSize - 1, ho));

	enc = EncodeCancel(77);
	CHECK(DecodeCancel(enc.data(), enc.size(), v) && v == 77);

	Error e;
	e.streamId = 5;
	e.code = kErrOpen;
	enc = EncodeError(e);
	Error eo;
	CHECK(DecodeError(enc.data(), enc.size(), eo) && eo.streamId == 5 && eo.code == kErrOpen);
	CHECK(!DecodeError(enc.data(), 7, eo));
}

static void TestSafePaths()
{
	const char* good[] = {
		"a.txt",
		"dir/sub/file.bin",
		"\xE4\xB8\xAD\xE6\x96\x87/\xE6\x96\x87\xE4\xBB\xB6.docx",
		"name with spaces.txt",
		".hidden",
	};
	for (const char* p : good)
		CHECK(IsSafeRelativePath(p));

	const char* bad[] = {
		"",
		"/abs",
		"\\abs",
		"C:/Windows",
		"C:file",
		"..",
		"../x",
		"a/../b",
		"a/./b",
		"a//b",
		"a/",
		"a\\b",
		"file.txt:stream",
		"a<b",
		"a>b",
		"a\"b",
		"a|b",
		"a?b",
		"a*b",
		"trailingdot.",
		"trailingspace ",
		"CON",
		"con",
		"nul.txt",
		"dir/AUX",
		"COM1",
		"lpt9.log",
		"COM\xC2\xB9",
		"CON .txt",
		"con.txt.bak/x",     // the part before the first dot is a device name
		"\x01name",
		"bad\xFFutf8",
		"\xC0\xAF",          // overlong '/'
		"\xED\xA0\x80",      // UTF-16 surrogate encoded in UTF-8
	};
	for (const char* p : bad) {
		if (IsSafeRelativePath(p))
			std::fprintf(stderr, "unexpectedly safe: %s\n", p);
		CHECK(!IsSafeRelativePath(p));
	}

	CHECK(IsSafeRelativePath("COM10"));  // only COM0-9 are devices
	CHECK(IsSafeRelativePath("console")); // only the exact name is reserved
	CHECK(IsSafeRelativePath("nul_file"));

	// Length limit counts UTF-16 units, not bytes.
	std::string longAscii(kMaxPathChars, 'a');
	CHECK(IsSafeRelativePath(longAscii));
	CHECK(!IsSafeRelativePath(longAscii + "a"));
	std::string longChinese;
	for (size_t i = 0; i < kMaxPathChars; ++i)
		longChinese += "\xE4\xB8\xAD"; // 1 UTF-16 unit, 3 bytes each -> 777 bytes
	CHECK(IsSafeRelativePath(longChinese));
	CHECK(!IsSafeRelativePath(longChinese + "a"));

	CHECK(Utf16Length("\xF0\x9F\x98\x80") == 2); // emoji = surrogate pair
	CHECK(Utf16Length("abc") == 3);
	CHECK(Utf16Length("\xE4\xB8") == SIZE_MAX); // truncated sequence
}

static void TestFilter()
{
	Offer o;
	const char* paths[] = { "ok", "ok/a.txt", "../evil", "ok/b.txt", "C:/x" };
	for (const char* p : paths) {
		Entry e;
		e.path = p;
		o.entries.push_back(e);
	}
	std::vector<uint32_t> idMap;
	CHECK(FilterUnsafeEntries(o, idMap) == 2);
	CHECK(o.entries.size() == 3);
	CHECK(idMap.size() == 3 && idMap[0] == 0 && idMap[1] == 1 && idMap[2] == 3);
	CHECK(o.entries[2].path == "ok/b.txt");
}

int main()
{
	TestOfferRoundTrip();
	TestSmallMessages();
	TestSafePaths();
	TestFilter();
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all fileclip_proto tests passed\n");
	return 0;
}
