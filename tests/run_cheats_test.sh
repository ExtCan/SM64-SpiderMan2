#!/usr/bin/env bash
# Cheats and poses against the real libsm64 + your own SM64 ROM.
#   SM2MARIO_TEST_ROM=/path/to/sm64.us.z64 tests/run_cheats_test.sh <libsm64 checkout built natively with `make lib`>
set -euo pipefail
cd "$(dirname "$0")/.."
LIB="${1:?path to a native libsm64 build (containing dist/libsm64.so)}"
mkdir -p build-tests
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
  -Ithird_party/libsm64 \
  tests/test_cheats.cpp src/mod/cheats.cpp src/mod/settings.cpp src/mod/config.cpp src/common/ini.cpp src/game/combat.cpp \
  -L"$LIB/dist" -lsm64 -Wl,-rpath,"$LIB/dist" -o build-tests/cheats_test
./build-tests/cheats_test
