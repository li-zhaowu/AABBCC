#!/bin/sh
# Cross-compile the single window end-to-end test with MinGW and run it in
# Wine (no Windows.Graphics.Capture there: exercises the PrintWindow path).
# zstd is replaced by a copy-only test double (zstd_stub/).
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
x86_64-w64-mingw32-gcc -c -O1 zstd_stub/zstd_stub.c -o "$T/zstd_stub.o"
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -D_WIN32_WINNT=0x0A00 -static \
	-I../.. -I../../winvnc/winvnc -Izstd_stub \
	window_e2e.cpp ../../winvnc/winvnc/vncWindowSession.cpp ../../winvnc/winvnc/vncWindowCapture.cpp \
	../../common/WindowProto.cpp ../../common/ExtChannel.cpp "$T/zstd_stub.o" \
	-ld3d11 -ldxgi -ldwmapi -lgdi32 -luser32 -lole32 -o "$T/window_e2e.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
RUN="env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE $T/window_e2e.exe $*"
if [ -z "$DISPLAY" ] && command -v xvfb-run >/dev/null; then
	exec xvfb-run -a $RUN
fi
exec $RUN
