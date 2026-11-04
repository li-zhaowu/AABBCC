#!/bin/sh
# End-to-end test of the file manager engines (server + client + transfer
# queue) through an in-process transport, cross-compiled with MinGW and run
# in Wine.
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -D_WIN32_WINNT=0x0A00 -static -I../.. \
	fileman_e2e_test.cpp ../../common/FileManProto.cpp ../../common/FileManServer.cpp \
	../../common/FileManClient.cpp ../../common/FileTransferQueue.cpp ../../common/ExtChannel.cpp \
	-lshell32 -lole32 -luuid -o "$T/fileman_e2e.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
exec env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE "$T/fileman_e2e.exe"
