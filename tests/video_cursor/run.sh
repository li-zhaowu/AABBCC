#!/bin/sh
# Cross-compile the cursor test with MinGW and run it in Wine.
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -D_WIN32_WINNT=0x0A00 -static -I../.. \
	video_cursor_test.cpp ../../vncviewer/VideoCursor.cpp -lgdi32 -luser32 -o "$T/video_cursor_test.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
RUN="env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE $T/video_cursor_test.exe"
if [ -z "$DISPLAY" ] && command -v xvfb-run >/dev/null; then
	exec xvfb-run -a $RUN
fi
exec $RUN
