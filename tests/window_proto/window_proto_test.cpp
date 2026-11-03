// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Unit tests for common/WindowProto. Build with tests/window_proto/run.sh or:
//   cl /std:c++17 /EHsc window_proto_test.cpp ..\..\common\WindowProto.cpp ..\..\common\ExtChannel.cpp zstd.lib

#include "../../common/WindowProto.h"

#include <cstdio>
#include <cstring>
#include <random>

using namespace window;

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

template <class T, class Enc, class Dec>
static void CheckTruncations(const T& value, Enc enc, Dec dec)
{
	auto bytes = enc(value);
	for (size_t n = 0; n < bytes.size(); ++n) {
		T t;
		CHECK(!dec(bytes.data(), n, t));
	}
}

static void TestMessages()
{
	std::vector<Entry> list(2);
	list[0].handle = 0x0000000100020003ull;
	list[0].pid = 1234;
	list[0].title = "Document1 - Word";
	list[0].program = "WINWORD.EXE";
	list[1].handle = 42;
	list[1].minimized = true;
	list[1].title = "\xE8\xAE\xA1\xE7\xAE\x97\xE5\x99\xA8"; // Chinese title
	list[1].program = "calc.exe";
	auto b = EncodeList(list);
	std::vector<Entry> lo;
	CHECK(DecodeList(b.data(), b.size(), lo));
	CHECK(lo.size() == 2 && lo[0].handle == list[0].handle && lo[0].pid == 1234 && !lo[0].minimized &&
	      lo[0].title == list[0].title && lo[0].program == "WINWORD.EXE" && lo[1].minimized &&
	      lo[1].title == list[1].title);
	CheckTruncations(list, EncodeList, DecodeList);
	std::string longTitle(2000, 'x');
	list[0].title = longTitle; // capped
	b = EncodeList(list);
	CHECK(DecodeList(b.data(), b.size(), lo) && lo[0].title.size() == kMaxText);

	uint64_t h = 0;
	uint8_t tr = 99;
	b = EncodeSelect(0xFFFFFFFF12345678ull);
	CHECK(DecodeSelect(b.data(), b.size(), h, tr) && h == 0xFFFFFFFF12345678ull && tr == kTransportTiles);
	CHECK(!DecodeSelect(b.data(), 7, h, tr));
	CHECK(DecodeSelect(b.data(), 8, h, tr) && tr == kTransportTiles); // older form: no transport byte
	b = EncodeSelect(42, kTransportVideo);
	CHECK(DecodeSelect(b.data(), b.size(), h, tr) && h == 42 && tr == kTransportVideo);
	b.back() = 7; // unknown transport
	CHECK(!DecodeSelect(b.data(), b.size(), h, tr));

	State st;
	st.sessionId = 9;
	st.state = kStateMinimized;
	st.message = "minimized";
	b = EncodeState(st);
	State so;
	CHECK(DecodeState(b.data(), b.size(), so) && so.sessionId == 9 && so.state == kStateMinimized &&
	      so.message == "minimized");
	CheckTruncations(st, EncodeState, DecodeState);
	CHECK(IsFinal(kStateClosed) && IsFinal(kStateEnded) && !IsFinal(kStateMinimized) && !IsFinal(kStateActive));

	Canvas c;
	c.sessionId = 3;
	c.width = 1280;
	c.height = 800;
	b = EncodeCanvas(c);
	Canvas co;
	CHECK(DecodeCanvas(b.data(), b.size(), co) && co.width == 1280 && co.height == 800 && co.sessionId == 3);
	CheckTruncations(c, EncodeCanvas, DecodeCanvas);
	c.width = 0;
	b = EncodeCanvas(c);
	CHECK(!DecodeCanvas(b.data(), b.size(), co));

	uint32_t s = 0, f = 0;
	b = EncodeAck(5, 77);
	CHECK(DecodeAck(b.data(), b.size(), s, f) && s == 5 && f == 77);
}

static std::vector<uint8_t> RandomPicture(int w, int h, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> v(size_t(w) * h * 4);
	for (auto& x : v)
		x = static_cast<uint8_t>(rng());
	return v;
}

static void TestTiles()
{
	const int w = 200, h = 130, stride = w * 4; // not multiples of 64
	auto a = RandomPicture(w, h, 1);
	auto all = ChangedTiles(nullptr, a.data(), w, h, stride);
	CHECK(all.size() == 4 * 3); // 64,64,64,8 x 64,64,2
	CHECK(all.back().w == 8 && all.back().h == 2 && all.back().x == 192 && all.back().y == 128);

	auto b = a;
	CHECK(ChangedTiles(a.data(), b.data(), w, h, stride).empty());
	b[(size_t(70) * w + 130) * 4] ^= 1;  // tile (2,1)
	b[(size_t(129) * w + 199) * 4 + 3] ^= 1; // last pixel: tile (3,2)
	auto ch = ChangedTiles(a.data(), b.data(), w, h, stride);
	CHECK(ch.size() == 2);
	if (ch.size() == 2) {
		CHECK(ch[0].x == 128 && ch[0].y == 64 && ch[0].w == 64 && ch[0].h == 64);
		CHECK(ch[1].x == 192 && ch[1].y == 128 && ch[1].w == 8 && ch[1].h == 2);
	}

	// Full round trip: pack, compress, frame, decode, decompress, unpack.
	auto packed = PackTiles(b.data(), stride, ch);
	CHECK(packed.size() == PackedSize(ch));
	auto comp = Compress(packed);
	CHECK(!comp.empty());
	FrameHeader fh;
	fh.sessionId = 1;
	fh.frameNo = 2;
	fh.canvasWidth = w;
	fh.canvasHeight = h;
	fh.tiles = ch;
	fh.rawSize = static_cast<uint32_t>(packed.size());
	auto msg = EncodeFrame(fh, comp);
	FrameHeader fo;
	CHECK(DecodeFrameHeader(msg.data(), msg.size(), fo));
	CHECK(fo.tiles == ch && fo.rawSize == packed.size() && fo.canvasWidth == w && fo.frameNo == 2);
	std::vector<uint8_t> raw;
	CHECK(Decompress(msg.data() + fo.dataOffset, msg.size() - fo.dataOffset, fo.rawSize, raw));
	auto target = a; // viewer had the old picture
	CHECK(UnpackTiles(raw.data(), raw.size(), fo.tiles, target.data(), w, h, stride));
	CHECK(target == b);

	// Corruption and hostile input.
	CHECK(!Decompress(comp.data(), comp.size(), packed.size() + 1, raw)); // wrong size
	std::vector<uint8_t> junk(100, 0xAB);
	CHECK(!Decompress(junk.data(), junk.size(), 1000, raw));
	std::vector<Rect> outside = { Rect{ 190, 0, 64, 64 } };
	std::vector<uint8_t> big(PackedSize(outside));
	CHECK(!UnpackTiles(big.data(), big.size(), outside, target.data(), w, h, stride));
	CHECK(!UnpackTiles(big.data(), big.size() - 1, outside, target.data(), 300, h, 300 * 4));
	fh.rawSize += 1; // header and tile list disagree
	msg = EncodeFrame(fh, comp);
	CHECK(!DecodeFrameHeader(msg.data(), msg.size(), fo));
	for (size_t n = 0; n < 20 + ch.size() * 8; ++n)
		CHECK(!DecodeFrameHeader(msg.data(), n, fo));

	// Typical UI content compresses well.
	std::vector<uint8_t> ui(size_t(800) * 600 * 4, 0xF0);
	auto uiTiles = ChangedTiles(nullptr, ui.data(), 800, 600, 800 * 4);
	auto uiPacked = PackTiles(ui.data(), 800 * 4, uiTiles);
	auto uiComp = Compress(uiPacked);
	std::printf("800x600 flat picture: %zu -> %zu bytes\n", uiPacked.size(), uiComp.size());
	CHECK(uiComp.size() < uiPacked.size() / 100);
}

int main()
{
	TestMessages();
	TestTiles();
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all window_proto tests passed\n");
	return 0;
}
