#!/bin/sh
# Build and run the video protocol unit tests (g++ or clang++).
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}/video_proto_test
$CXX -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_FLAGS} -pthread \
	video_proto_test.cpp ../../common/VideoProto.cpp ../../common/ExtChannel.cpp -o "$OUT"
"$OUT"
