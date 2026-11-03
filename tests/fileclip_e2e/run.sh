#!/bin/sh
# Cross-compile the file clipboard end-to-end test with MinGW and run it in
# Wine (needs: g++-mingw-w64-x86-64, wine64, xvfb when there is no display).
# On Windows, build with MSVC instead (see the .cpp header).
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/fileclip_e2e.exe
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -municode -DUNICODE -D_UNICODE \
	-D_WIN32_WINNT=0x0A00 -static \
	fileclip_e2e.cpp ../../common/FileClipboard.cpp ../../common/FileClipProto.cpp \
	../../common/ExtChannel.cpp -lole32 -loleaut32 -lshell32 -luuid -luser32 -ladvapi32 \
	-o "$OUT"

WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
# Wine needs a UTF-8 locale for non-ASCII file names. A custom TMPDIR makes
# xvfb-run + Wine abort on some systems, so drop it for the run.
RUN="env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE $OUT $*"
if [ -z "$DISPLAY" ] && command -v xvfb-run >/dev/null; then
	exec xvfb-run -a $RUN
fi
exec $RUN
