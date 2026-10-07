#!/usr/bin/env bash
# The crash guard under real faults (Wine): access violations, a call 2 bytes
# into a function, trashed callee-saved registers, nesting, and locks taken by
# code that faulted being released.
#   tests/run_guard_test.sh <work-dir>
set -euo pipefail
cd "$(dirname "$0")/.."
WORK="${1:?work dir}"
mkdir -p "$WORK"
x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -Wall -Wextra -Wno-array-bounds -static \
  tests/guard_test.cpp tests/guard_test_asm.S src/win/guard.cpp src/win/guard_x64.S src/common/log.cpp \
  src/common/platform.cpp -o "$WORK/guard_test.exe"
cd "$WORK"
WINEDEBUG=-all WINEPREFIX="$WORK/prefix" wine guard_test.exe
