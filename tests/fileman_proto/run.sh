#!/bin/sh
# Build and run the file manager protocol unit tests (g++ or clang++).
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}/fileman_proto_test
$CXX -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_FLAGS} -pthread \
	fileman_proto_test.cpp ../../common/FileManProto.cpp ../../common/ExtChannel.cpp -o "$OUT"
"$OUT"
