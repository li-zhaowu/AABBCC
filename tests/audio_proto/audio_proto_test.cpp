// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Unit tests for common/AudioProto. Build with tests/audio_proto/run.sh or:
//   cl /std:c++17 /EHsc audio_proto_test.cpp ..\..\common\AudioProto.cpp ..\..\common\ExtChannel.cpp

#include "../../common/AudioProto.h"

#include <cstdio>
#include <cstring>

using namespace audio;

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
	Start s;
	s.flags = 0;
	s.bitrateKbps = 96;
	auto b = EncodeStart(s);
	Start so;
	CHECK(DecodeStart(b.data(), b.size(), so) && so.codec == kCodecAac && so.flags == 0 && so.bitrateKbps == 96);
	CheckTruncations(s, EncodeStart, DecodeStart);
	CHECK(Start().flags == kStartMuteRemote); // remote muted by default

	Format f;
	f.streamId = 4;
	f.channels = 2;
	f.sampleRate = 44100;
	f.bitrateKbps = 160;
	f.config = { 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x10 };
	b = EncodeFormat(f);
	Format fo;
	CHECK(DecodeFormat(b.data(), b.size(), fo));
	CHECK(fo.streamId == 4 && fo.channels == 2 && fo.sampleRate == 44100 && fo.bitrateKbps == 160 && fo.config == f.config);
	CheckTruncations(f, EncodeFormat, DecodeFormat);
	f.channels = 6; // only mono / stereo are sent
	b = EncodeFormat(f);
	CHECK(!DecodeFormat(b.data(), b.size(), fo));

	DataHeader h;
	h.streamId = 1;
	h.seq = 77;
	h.timestampUs = 123456789012ull;
	const uint8_t frame[] = { 0x21, 0x10, 0x05 };
	b = EncodeData(h, frame, sizeof(frame));
	DataHeader ho;
	CHECK(DecodeDataHeader(b.data(), b.size(), ho) && ho.seq == 77 && ho.timestampUs == h.timestampUs);
	CHECK(b.size() == kDataHeaderSize + 3 && std::memcmp(b.data() + kDataHeaderSize, frame, 3) == 0);
	CHECK(!DecodeDataHeader(b.data(), kDataHeaderSize - 1, ho));

	Status st;
	st.code = kStatusDeviceLost;
	st.message = "no playback device";
	b = EncodeStatus(st);
	Status sto;
	CHECK(DecodeStatus(b.data(), b.size(), sto) && sto.code == kStatusDeviceLost && sto.message == st.message);
	CheckTruncations(st, EncodeStatus, DecodeStatus);
}

static void TestJitter()
{
	// Stereo, target 4800 frames (100 ms at 48 kHz), max 14400 (300 ms).
	JitterBuffer jb(2, 4800, 14400);
	std::vector<int16_t> in(960 * 2), out(480 * 2);
	auto fill = [&](int16_t v) { std::fill(in.begin(), in.end(), v); };

	// Waits for the target before playing.
	fill(1);
	for (int i = 0; i < 4; ++i)
		jb.Push(in.data(), 960);
	CHECK(jb.Buffered() == 3840);
	CHECK(jb.Pull(out.data(), 480) == 0 && out[0] == 0 && jb.Waiting());
	jb.Push(in.data(), 960); // 4800 = target
	CHECK(jb.Pull(out.data(), 480) == 480 && out[0] == 1 && out[959] == 1 && !jb.Waiting());

	// Runs dry: partial read, silence after it, then waits again.
	jb.Clear();
	for (int i = 0; i < 5; ++i)
		jb.Push(in.data(), 960);
	size_t got = 0;
	for (int i = 0; i < 10; ++i)
		got += jb.Pull(out.data(), 480);
	CHECK(got == 4800);
	CHECK(jb.Pull(out.data(), 480) == 0 && jb.Waiting() && jb.Underruns() == 1);

	// A burst beyond the maximum drops the oldest down to the target.
	jb.Clear();
	fill(7);
	for (int i = 0; i < 15; ++i)
		jb.Push(in.data(), 960); // 14400 = max: kept
	CHECK(jb.Buffered() == 14400 && jb.DroppedFrames() == 0);
	fill(9);
	jb.Push(in.data(), 960);     // 15360 > max: back to target
	CHECK(jb.Buffered() == 4800 && jb.DroppedFrames() == 15360 - 4800);
	CHECK(jb.Pull(out.data(), 480) == 480);
	// Newest data is kept: the last 960 frames pushed were 9.
	std::vector<int16_t> rest(4320 * 2);
	jb.Pull(rest.data(), 4320);
	CHECK(rest.back() == 9);

	// Partial read keeps sample order and pads with silence.
	JitterBuffer mono(1, 2, 100);
	const int16_t seq[] = { 1, 2, 3 };
	mono.Push(seq, 3);
	int16_t o[5];
	CHECK(mono.Pull(o, 5) == 3 && o[0] == 1 && o[2] == 3 && o[3] == 0 && o[4] == 0);
}

int main()
{
	TestMessages();
	TestJitter();
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all audio_proto tests passed\n");
	return 0;
}
