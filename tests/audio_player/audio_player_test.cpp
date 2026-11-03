// AudioPlayer smoke test: feeds an ADTS AAC file to the viewer's player at
// real-time pace and checks that it opens, plays and closes cleanly
// (including a second stream, as after a device change on the server).
//   audio_player_test.exe tone.aac

#include "vncviewer/AudioPlayer.h"

#include <chrono>
#include <cstdio>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s tone.aac\n", argv[0]);
		return 2;
	}
	FILE* f = fopen(argv[1], "rb");
	CHECK(f);
	std::vector<uint8_t> adts;
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		adts.insert(adts.end(), buf, buf + n);
	fclose(f);
	std::vector<std::vector<uint8_t>> frames;
	for (size_t p = 0; p + 7 <= adts.size();) {
		CHECK(adts[p] == 0xFF && (adts[p + 1] & 0xF0) == 0xF0);
		const size_t len = ((adts[p + 3] & 3) << 11) | (adts[p + 4] << 3) | (adts[p + 5] >> 5);
		const size_t hdr = (adts[p + 1] & 1) ? 7 : 9;
		CHECK(len > hdr && p + len <= adts.size());
		frames.emplace_back(adts.begin() + p + hdr, adts.begin() + p + len);
		p += len;
	}
	CHECK(frames.size() > 10);

	int logs = 0;
	AudioPlayer player([&](int level, const std::string& msg) {
		++logs;
		std::printf("[%d] %s\n", level, msg.c_str());
	});
	for (uint32_t stream = 1; stream <= 2; ++stream) {
		audio::Format fmt;
		fmt.streamId = stream;
		fmt.channels = 2;
		fmt.sampleRate = 48000;
		std::string error;
		CHECK(player.Open(fmt, error));
		for (const auto& fr : frames) {
			player.Push(stream, fr.data(), fr.size());
			player.Push(stream + 100, fr.data(), fr.size()); // other stream: ignored
			std::this_thread::sleep_for(std::chrono::microseconds(1024 * 1000000 / 48000));
		}
	}
	player.Close();
	player.Close(); // idempotent
	CHECK(logs >= 2);
	std::printf("audio player test passed (%zu frames per stream)\n", frames.size());
	return 0;
}
