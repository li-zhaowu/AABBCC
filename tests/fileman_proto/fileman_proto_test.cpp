// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Unit tests for common/FileManProto. Build with tests/fileman_proto/run.sh or:
//   cl /std:c++17 /EHsc fileman_proto_test.cpp ..\..\common\FileManProto.cpp ..\..\common\ExtChannel.cpp

#include "../../common/FileManProto.h"

#include <cstdio>
#include <cstring>
#include <functional>

using namespace fileman;

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

// Every strict prefix of a message must be rejected.
static void CheckTruncations(const std::vector<uint8_t>& bytes, const std::function<bool(const uint8_t*, size_t)>& dec)
{
	for (size_t n = 0; n < bytes.size(); ++n) {
		if (dec(bytes.data(), n)) {
			std::fprintf(stderr, "accepted truncated message of %zu/%zu bytes\n", n, bytes.size());
			++g_failures;
		}
	}
}

static void TestRequests()
{
	const std::string path = "C:\\Users\\\xE5\xBC\xA0\xE4\xB8\x89\\file.txt"; // 张三
	auto b = EncodePathRequest(7, path);
	uint32_t id = 0;
	std::string p;
	CHECK(DecodePathRequest(b.data(), b.size(), id, p) && id == 7 && p == path);
	CheckTruncations(b, [](const uint8_t* d, size_t n) { uint32_t i; std::string s; return DecodePathRequest(d, n, i, s); });

	b = EncodeRootsRequest(9);
	CHECK(DecodeRootsRequest(b.data(), b.size(), id) && id == 9);

	RenameRequest rn{ 3, "C:\\a", "C:\\b" };
	b = EncodeRename(rn);
	RenameRequest rn2;
	CHECK(DecodeRename(b.data(), b.size(), rn2) && rn2.reqId == 3 && rn2.from == "C:\\a" && rn2.to == "C:\\b");
	CheckTruncations(b, [](const uint8_t* d, size_t n) { RenameRequest r; return DecodeRename(d, n, r); });

	ReadRequest rd{ 4, 5, 0x123456789ULL, 4 * 1024 * 1024 };
	b = EncodeRead(rd);
	ReadRequest rd2;
	CHECK(DecodeRead(b.data(), b.size(), rd2) && rd2.reqId == 4 && rd2.handle == 5 && rd2.offset == 0x123456789ULL &&
	      rd2.length == 4u * 1024 * 1024);
	CheckTruncations(b, [](const uint8_t* d, size_t n) { ReadRequest r; return DecodeRead(d, n, r); });
	rd.length = 0;
	b = EncodeRead(rd);
	CHECK(!DecodeRead(b.data(), b.size(), rd2));
	rd.length = kMaxReadLength + 1;
	b = EncodeRead(rd);
	CHECK(!DecodeRead(b.data(), b.size(), rd2));

	b = EncodeHandleRequest(11, 12);
	uint32_t h = 0;
	CHECK(DecodeHandleRequest(b.data(), b.size(), id, h) && id == 11 && h == 12);

	OpenWriteRequest ow{ 13, "D:\\x\\y.bin", 5000000000ULL, kWriteOverwrite | kWriteResume, 123456789ULL };
	b = EncodeOpenWrite(ow);
	OpenWriteRequest ow2;
	CHECK(DecodeOpenWrite(b.data(), b.size(), ow2) && ow2.reqId == 13 && ow2.path == ow.path &&
	      ow2.size == 5000000000ULL && ow2.flags == (kWriteOverwrite | kWriteResume) && ow2.mtime == 123456789ULL);
	{
		// Without the time (older viewers) it is still accepted; shorter not.
		const size_t old = b.size() - 8;
		for (size_t k = 0; k < old; ++k)
			CHECK(!DecodeOpenWrite(b.data(), k, ow2));
		CHECK(DecodeOpenWrite(b.data(), old, ow2) && ow2.mtime == 0);
		CHECK(!DecodeOpenWrite(b.data(), old + 3, ow2)); // a cut-off time
	}
	OpenWriteReply owr{ 9, 777777ULL };
	b = EncodeOpenWriteReply(owr);
	OpenWriteReply owr2;
	CHECK(DecodeOpenWriteReply(b.data(), b.size(), owr2) && owr2.handle == 9 && owr2.offset == 777777ULL);
	CHECK(DecodeOpenWriteReply(b.data(), 4, owr2) && owr2.offset == 0);
	CHECK(!DecodeOpenWriteReply(b.data(), 3, owr2));
	// Temporary names: stable, case-insensitive for the path, specific to
	// the file version.
	const std::string t1 = ResumeTempName("C:\\Data\\A.bin", 100, 5);
	CHECK(t1 == ResumeTempName("c:\\data\\a.bin", 100, 5));
	CHECK(t1 != ResumeTempName("C:\\Data\\A.bin", 101, 5));
	CHECK(t1 != ResumeTempName("C:\\Data\\A.bin", 100, 6));
	CHECK(t1 != ResumeTempName("C:\\Data\\B.bin", 100, 5));
	CHECK(t1.size() == 27 && t1.rfind("~uvnc-", 0) == 0 && t1.substr(22) == ".part");

	std::vector<uint8_t> bytes(1000, 0xAB);
	b = EncodeWrite(WriteHeader{ 2, 77 }, bytes.data(), bytes.size());
	WriteHeader wh;
	CHECK(DecodeWriteHeader(b.data(), b.size(), wh) && wh.handle == 2 && wh.offset == 77 &&
	      b.size() == kWriteHeaderSize + bytes.size());
	CHECK(!DecodeWriteHeader(b.data(), kWriteHeaderSize - 1, wh));
	std::vector<uint8_t> big(kDataPieceBytes + 1);
	b = EncodeWrite(WriteHeader{ 2, 0 }, big.data(), big.size());
	CHECK(!DecodeWriteHeader(b.data(), b.size(), wh));

	CloseWriteRequest cw{ 14, 2, 1234, true };
	b = EncodeCloseWrite(cw);
	CloseWriteRequest cw2;
	CHECK(DecodeCloseWrite(b.data(), b.size(), cw2) && cw2.reqId == 14 && cw2.handle == 2 && cw2.mtime == 1234 && cw2.commit);
	CheckTruncations(b, [](const uint8_t* d, size_t n) { CloseWriteRequest r; return DecodeCloseWrite(d, n, r); });

	// Oversized path is cut when encoding, rejected when longer on the wire.
	std::string longPath(kMaxPathBytes + 10, 'a');
	b = EncodePathRequest(1, longPath);
	CHECK(DecodePathRequest(b.data(), b.size(), id, p) && p.size() == kMaxPathBytes);
	b[4] = 0xFF; b[5] = 0xFF; // length field 65535 > limit
	CHECK(!DecodePathRequest(b.data(), b.size(), id, p));
}

static void TestReplies()
{
	ReplyHeader rh{ 5, kErrAccess, kReplyMore, "Access is denied." };
	std::vector<Entry> entries(3);
	entries[0] = Entry{ "dir", kAttrDirectory, 0, 1 };
	entries[1] = Entry{ "a.txt", kAttrArchive | 0x8000 /* dropped */, 12345, 2 };
	entries[2] = Entry{ "\xE4\xB8\xAD.doc", kAttrReadOnly, 1ULL << 40, 3 };
	auto b = EncodeReply(rh, EncodeEntries(entries));
	ReplyHeader rh2;
	size_t off = 0;
	CHECK(DecodeReplyHeader(b.data(), b.size(), rh2, off) && rh2.reqId == 5 && rh2.status == kErrAccess &&
	      rh2.flags == kReplyMore && rh2.message == "Access is denied.");
	std::vector<Entry> got;
	CHECK(DecodeEntries(b.data() + off, b.size() - off, got) && got.size() == 3);
	CHECK(got[0].name == "dir" && got[0].IsDirectory());
	CHECK(got[1].attributes == kAttrArchive && got[1].size == 12345);
	CHECK(got[2].size == (1ULL << 40) && got[2].mtime == 3);
	const std::vector<uint8_t> body = EncodeEntries(entries);
	CheckTruncations(body, [](const uint8_t* d, size_t n) { std::vector<Entry> e; return DecodeEntries(d, n, e); });
	CheckTruncations(EncodeReply(rh, {}), [](const uint8_t* d, size_t n) { ReplyHeader h; size_t o; return DecodeReplyHeader(d, n, h, o); });

	std::vector<Root> roots(2);
	roots[0] = Root{ "C:\\", "System", kRootFixed, 500ULL << 30, 100ULL << 30 };
	roots[1] = Root{ "C:\\Users\\me\\Desktop", "Desktop", kRootFolder, 0, 0 };
	b = EncodeRoots(roots);
	std::vector<Root> roots2;
	CHECK(DecodeRoots(b.data(), b.size(), roots2) && roots2.size() == 2 && roots2[0].label == "System" &&
	      roots2[0].totalBytes == (500ULL << 30) && roots2[1].type == kRootFolder);
	CheckTruncations(b, [](const uint8_t* d, size_t n) { std::vector<Root> r; return DecodeRoots(d, n, r); });

	Entry st{ "", kAttrDirectory, 0, 99 };
	b = EncodeStat(st);
	Entry st2;
	CHECK(DecodeStat(b.data(), b.size(), st2) && st2.IsDirectory() && st2.mtime == 99);

	b = EncodeOpenReadReply(OpenReadReply{ 3, 1000, 55 });
	OpenReadReply orr;
	CHECK(DecodeOpenReadReply(b.data(), b.size(), orr) && orr.handle == 3 && orr.size == 1000 && orr.mtime == 55);

	std::vector<uint8_t> bytes(500, 1);
	b = EncodeData(DataHeader{ 3, 1000, kDataEof }, bytes.data(), bytes.size());
	DataHeader dh;
	CHECK(DecodeDataHeader(b.data(), b.size(), dh) && dh.handle == 3 && dh.offset == 1000 && dh.flags == kDataEof);
	CHECK(!DecodeDataHeader(b.data(), kDataHeaderSize - 1, dh));

	b = EncodeWriteAck(WriteAck{ 2, kErrDiskFull, 4096 });
	WriteAck wa;
	CHECK(DecodeWriteAck(b.data(), b.size(), wa) && wa.handle == 2 && wa.status == kErrDiskFull && wa.written == 4096);

	// Too many entries in one page.
	std::vector<uint8_t> evil;
	evil.push_back(0); evil.push_back(0); evil.push_back(0xFF); evil.push_back(0xFF);
	CHECK(!DecodeEntries(evil.data(), evil.size(), got));
}

static void TestPaths()
{
	const char* good[] = {
		"C:\\", "c:\\", "C:\\Windows", "C:\\a b\\c.d", "D:\\dir\\file.tar.gz", "\\\\server\\share",
		"\\\\server\\share\\dir\\f", "C:\\\xE6\x96\x87\xE4\xBB\xB6", "C:\\.git", "C:\\console", "C:\\COM0",
	};
	for (const char* g : good) {
		if (!IsAcceptablePath(g))
			std::fprintf(stderr, "rejected good path %s\n", g);
		CHECK(IsAcceptablePath(g));
	}
	const char* bad[] = {
		"", "C:", "C:dir", "\\dir", "dir\\file", "C:/dir", "C:\\dir\\", "C:\\a\\\\b", "C:\\..", "C:\\a\\..\\b",
		"C:\\.", "\\\\?\\C:\\x", "\\\\.\\PhysicalDrive0", "\\\\server", "\\\\server\\", "C:\\a:b", "C:\\a*",
		"C:\\a?", "C:\\a|b", "C:\\a<b", "C:\\a\"b", "C:\\dot.", "C:\\space ", "C:\\CON", "C:\\dir\\nul.txt",
		"C:\\lpt1", "C:\\COM9.log", "C:\\a\x01", "C:\\\xC0\xAF", "C:\\\xED\xA0\x80", "1:\\x",
	};
	for (const char* b : bad) {
		if (IsAcceptablePath(b))
			std::fprintf(stderr, "accepted bad path %s\n", b);
		CHECK(!IsAcceptablePath(b));
	}
	std::string deep = "C:";
	while (deep.size() < kMaxPathBytes + 10)
		deep += "\\abcdefgh";
	CHECK(!IsAcceptablePath(deep));

	CHECK(IsRootPath("C:\\") && IsRootPath("\\\\srv\\share") && IsRootPath("\\\\srv\\share\\"));
	CHECK(!IsRootPath("C:\\x") && !IsRootPath("\\\\srv\\share\\x"));

	CHECK(IsValidName("New folder") && IsValidName("a.b.c") && IsValidName("\xE6\x96\xB0"));
	CHECK(!IsValidName("") && !IsValidName("a\\b") && !IsValidName("a/b") && !IsValidName("..") &&
	      !IsValidName("aux") && !IsValidName("x.") && !IsValidName(std::string(256, 'x')));

	CHECK(JoinPath("C:\\", "a") == "C:\\a");
	CHECK(JoinPath("C:\\a", "b") == "C:\\a\\b");
	CHECK(ParentPath("C:\\a\\b") == "C:\\a");
	CHECK(ParentPath("C:\\a") == "C:\\");
	CHECK(ParentPath("C:\\").empty());
	CHECK(ParentPath("\\\\srv\\share\\x") == "\\\\srv\\share");
	CHECK(ParentPath("\\\\srv\\share").empty());
	CHECK(FileNamePart("C:\\a\\b.txt") == "b.txt");
	CHECK(FileNamePart("C:\\") == "C:\\");
	CHECK(std::strcmp(StatusName(kErrDiskFull), "disk full") == 0);
}

int main()
{
	TestRequests();
	TestReplies();
	TestPaths();
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all fileman_proto tests passed\n");
	return 0;
}
