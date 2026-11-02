#!/bin/sh
# Build and run the ExtChannel unit tests (Linux/macOS, g++ or clang++).
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}/ext_channel_test
$CXX -std=c++17 -O1 -g -Wall -Wextra -pthread ${EXTRA_FLAGS} \
	ext_channel_test.cpp ../../common/ExtChannel.cpp -o "$OUT"
"$OUT"
