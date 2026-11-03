// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Test of vncviewer/VideoDecoder (Media Foundation) on Windows or Wine.
// Input: an H.264 Annex B file with access unit delimiters, made of frames
// of one solid colour (see run.sh). Checks picture size and the decoded
// Y / U / V values, which catches stride and plane offset mistakes.
//
//   video_decoder_test.exe file.h264 width height Y U V

#include "../../vncviewer/VideoDecoder.h"
#include "../../common/VideoProto.h"

#include <cstdio>
#include <cstdlib>
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

// Split an Annex B stream at access unit delimiters (NAL type 9).
static std::vector<std::vector<uint8_t>> SplitAccessUnits(const std::vector<uint8_t>& s)
{
	std::vector<size_t> starts;
	for (size_t i = 0; i + 4 < s.size(); ++i) {
		if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 0 && s[i + 3] == 1 && (s[i + 4] & 0x1F) == 9)
			starts.push_back(i);
	}
	std::vector<std::vector<uint8_t>> aus;
	for (size_t k = 0; k < starts.size(); ++k) {
		const size_t end = k + 1 < starts.size() ? starts[k + 1] : s.size();
		aus.emplace_back(s.begin() + starts[k], s.begin() + end);
	}
	return aus;
}

static bool Near(int a, int b, int tol) { return a >= b - tol && a <= b + tol; }

int main(int argc, char** argv)
{
	if (argc < 7) {
		std::fprintf(stderr, "usage: %s file.h264 width height Y U V\n", argv[0]);
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
	auto aus = SplitAccessUnits(data);
	CHECK(aus.size() >= 10);

	VideoDecoder dec;
	std::string err;
	CHECK(dec.Open(video::kCodecH264, w, h, 30, err));
	if (!err.empty())
		std::fprintf(stderr, "open: %s\n", err.c_str());

	int pictures = 0, firstPictureAfter = -1;
	for (size_t i = 0; i < aus.size(); ++i) {
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
			}, err);
		if (!ok)
			std::fprintf(stderr, "decode %zu: %s\n", i, err.c_str());
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

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all video_decoder tests passed\n");
	return 0;
}
