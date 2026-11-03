#!/bin/sh
# Encode a solid colour test clip with ffmpeg, cross-compile the decoder
# test with MinGW and run it in Wine (needs ffmpeg with libx264, MinGW,
# wine64, GStreamer libav plugins for Wine's H.264 decoder, xvfb).
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
# Colour 0x3080C0 = R 48, G 128, B 192 -> BT.601 limited Y 112, U 168, V 88.
ffmpeg -v error -y -f lavfi -i "color=c=0x3080C0:s=640x360:r=30" -frames:v 30 \
	-c:v libx264 -profile:v baseline -tune zerolatency -pix_fmt yuv420p \
	-x264-params aud=1:keyint=1000 -f h264 "$T/solid.h264"
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -D_WIN32_WINNT=0x0A00 -static -I../.. \
	video_decoder_test.cpp ../../vncviewer/VideoDecoder.cpp \
	-lmfplat -lmfuuid -lole32 -luuid -o "$T/video_decoder_test.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
RUN="env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE $T/video_decoder_test.exe Z:$T/solid.h264 640 360 112 168 88"
if [ -z "$DISPLAY" ] && command -v xvfb-run >/dev/null; then
	exec xvfb-run -a $RUN
fi
exec $RUN
