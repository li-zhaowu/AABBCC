// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Test of common/AacCodec on Windows or Wine (see run.sh).
//   aac_test.exe tone.aac
// tone.aac: ADTS AAC-LC, 48 kHz stereo, a 1 kHz sine of amplitude 1/8.
// Decodes it and checks length, frequency and level; if an AAC encoder is
// available (Windows; not in Wine), also encodes a tone and decodes it back.

#include "../../common/AacCodec.h"
#include "../../common/AudioProto.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

// Frequency from zero crossings of the left channel, and peak level.
static void Analyse(const std::vector<int16_t>& pcm, int rate, double& freq, double& peak)
{
	const size_t frames = pcm.size() / 2;
	const size_t from = frames / 4, to = frames * 3 / 4; // skip codec start-up
	int crossings = 0;
	peak = 0;
	for (size_t i = from + 1; i < to; ++i) {
		const int a = pcm[(i - 1) * 2], b = pcm[i * 2];
		if ((a < 0) != (b < 0))
			++crossings;
		peak = (std::max)(peak, std::fabs(b / 32768.0));
	}
	freq = crossings / 2.0 / (double(to - from) / rate);
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s tone.aac\n", argv[0]);
		return 2;
	}
	FILE* f = fopen(argv[1], "rb");
	if (!f) {
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}
	std::vector<uint8_t> adts;
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		adts.insert(adts.end(), buf, buf + n);
	fclose(f);

	// Split ADTS into raw frames (header 7 bytes, or 9 with CRC).
	std::vector<std::vector<uint8_t>> frames;
	for (size_t p = 0; p + 7 <= adts.size();) {
		CHECK(adts[p] == 0xFF && (adts[p + 1] & 0xF0) == 0xF0);
		const bool crc = !(adts[p + 1] & 1);
		const size_t len = ((adts[p + 3] & 3) << 11) | (adts[p + 4] << 3) | (adts[p + 5] >> 5);
		const size_t hdr = crc ? 9 : 7;
		if (len <= hdr || p + len > adts.size())
			break;
		frames.emplace_back(adts.begin() + p + hdr, adts.begin() + p + len);
		p += len;
	}
	std::printf("%zu AAC frames in the file\n", frames.size());
	CHECK(frames.size() > 40);

	AacDecoder dec;
	std::string err;
	CHECK(dec.Open(48000, 2, {}, err));
	if (!err.empty())
		std::fprintf(stderr, "decoder: %s\n", err.c_str());
	std::vector<int16_t> pcm;
	for (const auto& fr : frames)
		if (!dec.Decode(fr.data(), fr.size(), pcm, err))
			std::fprintf(stderr, "decode: %s\n", err.c_str());
	double freq = 0, peak = 0;
	Analyse(pcm, 48000, freq, peak);
	std::printf("decoded %zu frames of audio, %.1f Hz, peak %.3f\n", pcm.size() / 2, freq, peak);
	CHECK(pcm.size() / 2 >= 46000 && pcm.size() / 2 <= 50000);
	CHECK(freq > 980 && freq < 1020);
	CHECK(peak > 0.06 && peak < 0.2); // tone of amplitude 1/8 (decoders differ slightly)

	// Our own encoder (not available in Wine).
	AacEncoder enc;
	if (!enc.Open(48000, 2, 128, err)) {
		std::printf("AAC encoder not available here (%s): encoder round trip skipped\n", err.c_str());
	} else {
		std::vector<int16_t> tone(48000 * 2);
		for (int i = 0; i < 48000; ++i)
			tone[i * 2] = tone[i * 2 + 1] = static_cast<int16_t>(4096 * std::sin(2 * 3.14159265358979 * 440 * i / 48000));
		std::vector<std::vector<uint8_t>> out;
		for (int i = 0; i < 48000; i += 480)
			CHECK(enc.Encode(&tone[i * 2], 480, out, err));
		std::printf("encoder: %zu frames, config %zu bytes, %u kbps\n", out.size(), enc.Config().size(), enc.BitrateKbps());
		AacDecoder dec2;
		CHECK(dec2.Open(48000, 2, enc.Config(), err));
		std::vector<int16_t> back;
		for (const auto& fr : out)
			dec2.Decode(fr.data(), fr.size(), back, err);
		Analyse(back, 48000, freq, peak);
		std::printf("round trip: %.1f Hz, peak %.3f\n", freq, peak);
		CHECK(freq > 430 && freq < 450);
	}

	// The configuration we build ourselves (AudioSpecificConfig 0x11 0x90
	// for LC, 48 kHz, stereo).
	const std::vector<uint8_t> ud = AacUserData(48000, 2);
	CHECK(ud.size() == 14 && ud[12] == 0x11 && ud[13] == 0x90);

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all aac_codec tests passed\n");
	return 0;
}
