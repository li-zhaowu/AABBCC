#!/bin/sh
# Remote files as "virtual files" (what Explorer reads when remote items are
# dragged out of the file transfer window): MinGW + Wine.
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -static \
	-I../.. -I../../vncviewer fileman_dnd_test.cpp ../../vncviewer/FileManagerDragDrop.cpp \
	../../common/FileManProto.cpp ../../common/FileManServer.cpp ../../common/FileManClient.cpp \
	../../common/ExtChannel.cpp -lshell32 -lole32 -luuid -o "$T/fileman_dnd.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
exec env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE "$T/fileman_dnd.exe"
