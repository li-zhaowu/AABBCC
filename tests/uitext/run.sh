#!/bin/sh
# Translations of the custom viewer texts: every Tr()/TrF() text has a
# Chinese translation (gen.py fails otherwise), and the lookup works.
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}
python3 ../../vncviewer/uitext/gen.py
x86_64-w64-mingw32-g++ -std=c++17 -O1 -g -Wall -Wextra -DUNICODE -D_UNICODE -static -I../../vncviewer \
	uitext_test.cpp ../../vncviewer/UiText.cpp -o "$T/uitext_test.exe"
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
exec env -u TMPDIR LC_ALL=C.UTF-8 WINEDEBUG=${WINEDEBUG:--all} $WINE "$T/uitext_test.exe"
