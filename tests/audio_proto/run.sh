#!/bin/sh
# Build and run the audio protocol / jitter buffer unit tests.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}/audio_proto_test
$CXX -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_FLAGS} -pthread \
	audio_proto_test.cpp ../../common/AudioProto.cpp ../../common/ExtChannel.cpp -o "$OUT"
"$OUT"
