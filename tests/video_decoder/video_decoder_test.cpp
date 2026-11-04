// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Test of vncviewer/VideoDecoder (Media Foundation) on Windows or Wine.
// Input: frames of one solid colour (see run.sh), as
//   .h264 / .h265  Annex B with access unit delimiters, or
//   .obu           AV1 low overhead OBU stream with temporal delimiters
//                  (what NVENC sends).
// Checks picture size and the decoded Y / U / V values, which catches
// stride and plane offset mistakes.
//
//   video_decoder_test.exe file.h264|file.h265|file.obu width height Y U V
//
// Wine has no HEVC or AV1 decoder; run those on Windows (see run.sh).

#include "../../vncviewer/VideoDecoder.h"
#include "../../common/VideoProto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

// Split an Annex B stream at access unit delimiters (H.264 NAL type 9,
// HEVC NAL type 35).
static std::vector<std::vector<uint8_t>> SplitAccessUnits(const std::vector<uint8_t>& s, bool hevc)
{
	std::vector<size_t> starts;
	for (size_t i = 0; i + 4 < s.size(); ++i) {
		if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 0 && s[i + 3] == 1 &&
		    (hevc ? ((s[i + 4] >> 1) & 0x3F) == 35 : (s[i + 4] & 0x1F) == 9))
			starts.push_back(i);
	}
	std::vector<std::vector<uint8_t>> aus;
	for (size_t k = 0; k < starts.size(); ++k) {
		const size_t end = k + 1 < starts.size() ? starts[k + 1] : s.size();
		aus.emplace_back(s.begin() + starts[k], s.begin() + end);
	}
	return aus;
}

// Split an AV1 OBU stream (every OBU with a size field) into temporal
// units, each starting with a temporal delimiter OBU (type 2).
static std::vector<std::vector<uint8_t>> SplitTemporalUnits(const std::vector<uint8_t>& s)
{
	std::vector<size_t> starts;
	size_t i = 0;
	while (i < s.size()) {
		const uint8_t header = s[i];
		const int type = (header >> 3) & 0x0F;
		size_t p = i + 1 + ((header & 0x04) ? 1 : 0); // extension byte
		if (!(header & 0x02) || p >= s.size())
			break; // no size field: not a low overhead stream
		uint64_t size = 0;
		for (int k = 0; k < 8 && p < s.size(); ++k) {
			const uint8_t b = s[p++];
			size |= uint64_t(b & 0x7F) << (7 * k);
			if (!(b & 0x80))
				break;
		}
		if (type == 2)
			starts.push_back(i);
		if (size > s.size() - p)
			break;
		i = p + size_t(size);
	}
	std::vector<std::vector<uint8_t>> tus;
	for (size_t k = 0; k < starts.size(); ++k) {
		const size_t end = k + 1 < starts.size() ? starts[k + 1] : s.size();
		tus.emplace_back(s.begin() + starts[k], s.begin() + end);
	}
	return tus;
}

static bool EndsWith(const char* s, const char* suffix)
{
	const size_t a = strlen(s), b = strlen(suffix);
	return a >= b && _stricmp(s + a - b, suffix) == 0;
}

static bool Near(int a, int b, int tol) { return a >= b - tol && a <= b + tol; }

int main(int argc, char** argv)
{
	if (argc < 7) {
		std::fprintf(stderr, "usage: %s file.h264|file.h265|file.obu width height Y U V\n", argv[0]);
		return 2;
	}
	const int w = atoi(argv[2]), h = atoi(argv[3]);
	const int wantY = atoi(argv[4]), wantU = atoi(argv[5]), wantV = atoi(argv[6]);

	FILE* f = fopen(argv[1], "rb");
	if (!f) {
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}
	std::vector<uint8_t> data;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + n);
	fclose(f);
	const uint8_t codec = EndsWith(argv[1], ".obu") ? video::kCodecAv1
		: (EndsWith(argv[1], ".h265") || EndsWith(argv[1], ".hevc")) ? video::kCodecHevc : video::kCodecH264;
	auto aus = codec == video::kCodecAv1 ? SplitTemporalUnits(data) : SplitAccessUnits(data, codec == video::kCodecHevc);
	std::printf("%s: %zu frames\n", video::CodecName(codec), aus.size());
	CHECK(aus.size() >= 10);

	for (int pass = 0; pass < 2; ++pass) {
	// Pass 0 allows GPU decoding (falls back to software where there is no
	// usable GPU, e.g. under Wine); pass 1 forces software decoding.
	const bool allowHw = pass == 0;
	VideoDecoder dec;
	std::string err;
	CHECK(dec.Open(codec, w, h, 30, allowHw, err));
	if (!err.empty())
		std::fprintf(stderr, "open: %s\n", err.c_str());
	std::printf("pass %d: %s decoding\n", pass, dec.IsHardware() ? "GPU" : "software");
	if (!allowHw)
		CHECK(!dec.IsHardware());

	int pictures = 0, firstPictureAfter = -1;
	bool fellBack = false;
	for (size_t i = 0; i < aus.size(); ++i) {
		std::string decodeError;
		const bool ok = dec.Decode(aus[i].data(), aus[i].size(), static_cast<int64_t>(i) * 333333,
			[&](const VideoDecoder::Picture& p) {
				if (pictures++ == 0) {
					firstPictureAfter = static_cast<int>(i);
					std::printf("picture %dx%d pitch %d: Y=%d U=%d V=%d (centre)\n", p.width, p.height,
						p.pitch, p.y[(h / 2) * p.pitch + w / 2], p.uv[(h / 4) * p.pitch + (w / 2 & ~1)],
						p.uv[(h / 4) * p.pitch + (w / 2 & ~1) + 1]);
				}
				CHECK(p.width == w && p.height == h);
				// Last row of Y, last row of UV: catches a wrong UV offset.
				const int y = p.y[(h - 1) * p.pitch + w - 1];
				const int u = p.uv[(h / 2 - 1) * p.pitch + (w - 2)];
				const int v = p.uv[(h / 2 - 1) * p.pitch + (w - 1)];
				CHECK(Near(y, wantY, 4));
				CHECK(Near(u, wantU, 4));
				CHECK(Near(v, wantV, 4));
			}, decodeError);
		if (!ok && dec.IsHardware() && !fellBack) {
			// What the viewer does: GPU decoding failed, continue in
			// software from the next keyframe (here: the start).
			std::printf("GPU decoding failed (%s), falling back to software\n", decodeError.c_str());
			fellBack = true;
			CHECK(dec.Open(codec, w, h, 30, false, err));
			pictures = 0;
			firstPictureAfter = -1;
			i = static_cast<size_t>(-1);
			continue;
		}
		if (!ok)
			std::fprintf(stderr, "decode %zu: %s\n", i, decodeError.c_str());
		CHECK(ok);
	}
	std::printf("%zu access units, %d pictures, first picture after unit %d\n", aus.size(), pictures,
		firstPictureAfter);
	CHECK(pictures == static_cast<int>(aus.size()));
	CHECK(firstPictureAfter == 0); // low latency: no frames held back

	// Garbage must be reported, not crash.
	std::vector<uint8_t> junk(1000, 0x55);
	junk[0] = 0; junk[1] = 0; junk[2] = 0; junk[3] = 1; junk[4] = 0x65;
	dec.Decode(junk.data(), junk.size(), 0, [](const VideoDecoder::Picture&) {}, err);
	}
	for (uint8_t c : { uint8_t(video::kCodecH264), uint8_t(video::kCodecHevc), uint8_t(video::kCodecAv1) })
		std::printf("GPU %s decoding available: %s\n", video::CodecName(c),
			VideoDecoder::HardwareAvailable(c) ? "yes" : "no");

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all video_decoder tests passed\n");
	return 0;
}
