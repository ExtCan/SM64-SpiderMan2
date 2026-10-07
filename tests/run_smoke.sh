#!/usr/bin/env bash
# End-to-end smoke test under Wine + Xvfb + lavapipe (software Vulkan):
# test build of sm2mario.dll + fake sm64.dll + mock game, scripted play session.
#   tests/run_smoke.sh <work-dir> [default|MOCK_BAD_SETPOS|MOCK_CRASH_SETPOS]
set -euo pipefail
cd "$(dirname "$0")/.."
WORK="${1:?work dir}"
VARIANT="${2:-default}"
ROOT="$(pwd)"

cmake -S . -B build-test-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release \
  -DSM2MARIO_TESTING=ON >/dev/null
cmake --build build-test-mingw >/dev/null

GAME="$WORK/game-$VARIANT"
rm -rf "$GAME" && mkdir -p "$GAME/scripts/resources/sm2mario" "$GAME/sm2mario"
DEF=""
[ "$VARIANT" != "default" ] && DEF="-D$VARIANT"
x86_64-w64-mingw32-g++-posix -std=c++17 -O1 -static $DEF tests/smoke_host.cpp tests/mock_game.S \
  -o "$GAME/SmokeGame.exe" -ld3d12 -ldxgi
x86_64-w64-mingw32-gcc -O1 -shared -static-libgcc -Ithird_party/libsm64 tests/fake_sm64.c \
  -o "$GAME/scripts/resources/sm2mario/sm64.dll" -lm
cp build-test-mingw/sm2mario.dll "$GAME/scripts/"
cp package/resources/sm2mario/*.ini package/resources/sm2mario/*.txt "$GAME/scripts/resources/sm2mario/"
echo sm2mario.dll > "$GAME/scripts.txt"
head -c 65536 /dev/urandom > "$GAME/sm2mario/fake.z64"
# (The host punches with E: 0.3's second punch key, kept in this 0.4 file.)
printf '[Debug]\nShowOverlay = true\n[Combat]\nLogHits = true\n[General]\nLogLevel = info\nSettingsVersion = 0.4.0\n[Controls]\nKeyPunch = LMB, E\n' > "$GAME/sm2mario/sm2mario.ini"

export WINEDEBUG=-all WINEPREFIX="$WORK/prefix" VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export DISPLAY=:96 SM2MARIO_FAKE_ROM=1
pgrep -f "Xvfb :96" >/dev/null || (Xvfb :96 -screen 0 1280x720x24 >/dev/null 2>&1 &)
sleep 1
cd "$GAME"
rm -f shoot_now
(timeout 120 wine SmokeGame.exe 16 --d3d12 --mock --script > host.out 2>&1 &)
for i in $(seq 1 90); do sleep 1; [ -f shoot_now ] && break; done
xwd -root -display :96 -out shot.xwd && convert shot.xwd "$WORK/smoke-$VARIANT.png" && rm -f shot.xwd
for i in $(seq 1 60); do sleep 1; grep -q "host: done" host.out 2>/dev/null && break; done
echo "=== host ($VARIANT)"; grep -v fixme host.out | grep "host:" || true
echo "=== log ($VARIANT)"; cat sm2mario/sm2mario.log
