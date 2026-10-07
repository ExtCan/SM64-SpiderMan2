#!/usr/bin/env bash
# Builds and runs the host unit tests (Linux/macOS/MSYS2).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-tests
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread -fsanitize=address,undefined \
  -Ithird_party/libsm64 \
  tests/test_main.cpp \
  src/common/ini.cpp src/common/rom.cpp src/common/sha1.cpp src/common/platform.cpp src/common/log.cpp \
  src/game/pattern.cpp src/game/camera_finder.cpp src/game/combat.cpp src/game/rtti.cpp src/mod/follow_monitor.cpp src/mod/camera_lead.cpp src/mod/camera_override.cpp src/mod/settings.cpp \
  src/sm64/mario.cpp src/world/coords.cpp src/world/collision.cpp src/mod/config.cpp src/render/overlay.cpp \
  src/world/world_model.cpp src/world/physics_world.cpp src/world/surface_types.cpp src/render/view_constants.cpp src/render/d3d12_parse.cpp src/render/frame_policy.cpp src/render/stencil_census.cpp \
  -o build-tests/sm2mario_tests
./build-tests/sm2mario_tests
