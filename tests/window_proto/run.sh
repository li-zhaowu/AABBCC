#!/bin/sh
# Build and run the window protocol / tile unit tests (needs libzstd-dev).
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}/window_proto_test
$CXX -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_FLAGS} -pthread \
	window_proto_test.cpp ../../common/WindowProto.cpp ../../common/ExtChannel.cpp -lzstd -o "$OUT"
"$OUT"
