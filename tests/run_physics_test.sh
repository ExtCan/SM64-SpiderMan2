#!/usr/bin/env bash
# Real libsm64 + your own SM64 ROM walking on collision built from ray casts
# (the game physics source).
#   SM2MARIO_TEST_ROM=/path/to/sm64.us.z64 tests/run_physics_test.sh <libsm64 checkout built natively with `make lib`>
set -euo pipefail
cd "$(dirname "$0")/.."
LIB="${1:?path to a native libsm64 build (containing dist/libsm64.so)}"
mkdir -p build-tests
${CXX:-g++} -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter \
  -Ithird_party/libsm64 \
  tests/test_physics_mario.cpp src/world/coords.cpp src/world/collision.cpp src/world/physics_world.cpp \
  src/world/surface_types.cpp \
  -L"$LIB/dist" -lsm64 -Wl,-rpath,"$LIB/dist" -o build-tests/physics_mario_test
./build-tests/physics_mario_test
