// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Unit tests for common/VideoProto. Build with tests/video_proto/run.sh or:
//   cl /std:c++17 /EHsc video_proto_test.cpp ..\..\common\VideoProto.cpp ..\..\common\ExtChannel.cpp

#include "../../common/VideoProto.h"

#include <cstdio>
#include <cstring>

using namespace video;

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
		if (dec(bytes.data(), n, t))
			std::fprintf(stderr, "accepted truncated message of %zu/%zu bytes\n", n, bytes.size());
		CHECK(!dec(bytes.data(), n, t));
	}
}

int main()
{
	Start s;
	s.codec = kCodecHevc;
	s.maxFps = 30;
	s.maxBitrateKbps = 123456;
	s.maxWidth = 3840;
	s.maxHeight = 2160;
	auto b = EncodeStart(s);
	Start so;
	CHECK(DecodeStart(b.data(), b.size(), so));
	CHECK(so.codec == kCodecHevc && so.maxFps == 30 && so.maxBitrateKbps == 123456 &&
	      so.maxWidth == 3840 && so.maxHeight == 2160);
	CheckTruncations(s, EncodeStart, DecodeStart);
	s.maxFps = 0;
	b = EncodeStart(s);
	CHECK(!DecodeStart(b.data(), b.size(), so));

	Config c;
	c.streamId = 7;
	c.width = 2560;
	c.height = 1440;
	c.x = -1920; // monitor left of the framebuffer origin
	c.y = 200;
	c.fps = 60;
	c.bitrateKbps = 20000;
	b = EncodeConfig(c);
	Config co;
	CHECK(DecodeConfig(b.data(), b.size(), co));
	CHECK(co.streamId == 7 && co.width == 2560 && co.height == 1440 && co.x == -1920 &&
	      co.y == 200 && co.fps == 60 && co.bitrateKbps == 20000 && co.codec == kCodecH264);
	CheckTruncations(c, EncodeConfig, DecodeConfig);

	FrameHeader h;
	h.streamId = 3;
	h.frameNo = 0xFFFFFFF0;
	h.captureTimeUs = 0x0123456789ABCDEFull;
	h.flags = kFrameKey;
	const uint8_t nal[] = { 0, 0, 0, 1, 0x67, 0x42 };
	b = EncodeFrame(h, nal, sizeof(nal));
	CHECK(b.size() == kFrameHeaderSize + sizeof(nal));
	FrameHeader ho;
	CHECK(DecodeFrameHeader(b.data(), b.size(), ho));
	CHECK(ho.streamId == 3 && ho.frameNo == 0xFFFFFFF0 && ho.captureTimeUs == h.captureTimeUs &&
	      ho.flags == kFrameKey);
	CHECK(std::memcmp(b.data() + kFrameHeaderSize, nal, sizeof(nal)) == 0);
	CHECK(!DecodeFrameHeader(b.data(), kFrameHeaderSize - 1, ho));

	Feedback f;
	f.streamId = 9;
	f.lastFrameNo = 1000;
	f.decodeUs = 4321;
	f.flags = kFeedbackKeyframe;
	b = EncodeFeedback(f);
	Feedback fo;
	CHECK(DecodeFeedback(b.data(), b.size(), fo));
	CHECK(fo.streamId == 9 && fo.lastFrameNo == 1000 && fo.decodeUs == 4321 && fo.flags == kFeedbackKeyframe);
	CheckTruncations(f, EncodeFeedback, DecodeFeedback);

	Status st;
	st.code = kStatusUnsupported;
	st.message = "no NVIDIA encoder";
	b = EncodeStatus(st);
	Status sto;
	CHECK(DecodeStatus(b.data(), b.size(), sto));
	CHECK(sto.code == kStatusUnsupported && sto.message == st.message);
	CheckTruncations(st, EncodeStatus, DecodeStatus);
	st.message.assign(5000, 'x'); // capped on encode
	b = EncodeStatus(st);
	CHECK(DecodeStatus(b.data(), b.size(), sto) && sto.message.size() == 1024);

	// Cursor shapes.
	Cursor cu;
	cu.type = kCursorColor;
	cu.width = 32;
	cu.height = 32;
	cu.hotX = 5;
	cu.hotY = 6;
	cu.pitch = 128;
	cu.data.assign(128 * 32, 0x7F);
	b = EncodeCursor(cu);
	Cursor cuo;
	CHECK(DecodeCursor(b.data(), b.size(), cuo));
	CHECK(cuo.type == kCursorColor && cuo.width == 32 && cuo.hotY == 6 && cuo.data == cu.data);
	CHECK(!DecodeCursor(b.data(), b.size() - 1, cuo)); // data size must match
	Cursor mono;
	mono.type = kCursorMonochrome;
	mono.width = 32;
	mono.height = 32;
	mono.pitch = 4;
	mono.data.assign(4 * 64, 0xFF); // AND + XOR rows
	b = EncodeCursor(mono);
	CHECK(DecodeCursor(b.data(), b.size(), cuo) && cuo.data.size() == 256);
	mono.data.resize(4 * 32); // only one mask
	b = EncodeCursor(mono);
	CHECK(!DecodeCursor(b.data(), b.size(), cuo));
	Cursor hidden;
	b = EncodeCursor(hidden);
	CHECK(DecodeCursor(b.data(), b.size(), cuo) && cuo.type == kCursorHidden);
	cu.hotX = 40; // hotspot outside the shape
	b = EncodeCursor(cu);
	CHECK(!DecodeCursor(b.data(), b.size(), cuo));
	cu.hotX = 5;
	cu.width = 300; // too big
	cu.pitch = 1200;
	cu.data.assign(1200 * 32, 0);
	b = EncodeCursor(cu);
	CHECK(!DecodeCursor(b.data(), b.size(), cuo));

	// Clipping of the picture into the framebuffer.
	ClipResult r;
	Config k;
	k.width = 1920;
	k.height = 1080;
	k.x = 0;
	k.y = 0;
	CHECK(ClipToFramebuffer(k, 1920, 1080, r));
	CHECK(r.srcX == 0 && r.srcY == 0 && r.dstX == 0 && r.dstY == 0 && r.width == 1920 && r.height == 1080);
	k.x = 1920; // second monitor of a 3840 wide desktop
	CHECK(ClipToFramebuffer(k, 3840, 1080, r));
	CHECK(r.dstX == 1920 && r.srcX == 0 && r.width == 1920);
	k.x = -100; // partly left of the framebuffer
	k.y = 50;
	CHECK(ClipToFramebuffer(k, 1920, 1080, r));
	CHECK(r.srcX == 100 && r.dstX == 0 && r.width == 1820 && r.srcY == 0 && r.dstY == 50 && r.height == 1030);
	k.x = 2000; // entirely outside
	CHECK(!ClipToFramebuffer(k, 1920, 1080, r));

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all video_proto tests passed\n");
	return 0;
}
