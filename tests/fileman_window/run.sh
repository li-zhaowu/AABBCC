#!/bin/sh
# Opens the file transfer window under Wine against an in-process server,
# starts a (slowed down) upload and saves a screenshot of the window as a
# BMP: $TMPDIR/fm_window.bmp, and of the media settings dialog:
# $TMPDIR/media_settings.bmp. UVNC_UI_LANG=zh shows the Chinese texts. A smoke test of the UI code, not a pass/fail
# check of the looks (Wine has neither Segoe UI nor the icon font).
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
x86_64-w64-mingw32-windres app.rc -O coff -o "$T/fm_window_res.o"
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -static \
	-I../.. -I../../vncviewer fm_window_test.cpp ../../vncviewer/FileManagerWindow.cpp \
	../../vncviewer/FileManagerDragDrop.cpp ../../vncviewer/UiText.cpp ../../vncviewer/MediaSettings.cpp ../../common/FileManProto.cpp ../../common/FileManServer.cpp \
	../../common/FileManClient.cpp ../../common/FileTransferQueue.cpp ../../common/ExtChannel.cpp \
	"$T/fm_window_res.o" -mwindows -municode -lcomctl32 -lshlwapi -lshell32 -lole32 -luuid -lgdi32 \
	-o "$T/fm_window_test.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
RUN="env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} UVNC_UI_LANG=${UVNC_UI_LANG:-en} $WINE $T/fm_window_test.exe Z:$T/fm_window.bmp Z:$T/media_settings.bmp"
if [ -z "$DISPLAY" ] && command -v xvfb-run >/dev/null; then
	exec xvfb-run -a -s "-screen 0 1400x900x24" $RUN
fi
exec $RUN
