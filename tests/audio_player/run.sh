#!/bin/sh
# AudioPlayer smoke test (Wine; needs the GStreamer libav plugins for AAC).
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
ffmpeg -v error -y -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=1" -ac 2 -c:a aac -b:a 128k \
	-f adts "$T/tone.aac"
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -D_WIN32_WINNT=0x0A00 -static -I../.. \
	audio_player_test.cpp ../../vncviewer/AudioPlayer.cpp ../../common/AacCodec.cpp ../../common/AudioProto.cpp \
	../../common/ExtChannel.cpp -lmfplat -lmfuuid -lole32 -luuid -o "$T/audio_player_test.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
exec env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE "$T/audio_player_test.exe" "Z:$T/tone.aac"
