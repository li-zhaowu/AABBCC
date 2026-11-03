#!/bin/sh
# Build and run the bitrate adaptation unit tests (g++ or clang++).
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}/rate_controller_test
$CXX -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_FLAGS} rate_controller_test.cpp ../../common/RateController.cpp -o "$OUT"
"$OUT"
