#!/usr/bin/env bash
# Checks CollisionBuilder output against the real libsm64 collision code.
#   tests/run_libsm64_test.sh <libsm64 checkout built natively with `make lib`>
set -euo pipefail
cd "$(dirname "$0")/.."
LIB="${1:?path to a native libsm64 build (containing dist/libsm64.so)}"
mkdir -p build-tests
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
  -Ithird_party/libsm64 \
  tests/test_libsm64_collision.cpp src/world/coords.cpp src/world/collision.cpp \
  -L"$LIB/dist" -lsm64 -Wl,-rpath,"$LIB/dist" -o build-tests/libsm64_collision_test
./build-tests/libsm64_collision_test
