#!/usr/bin/env bash
# End-to-end test: the real mod (release build), libsm64's sm64.dll with your
# own SM64 ROM, and tests/deferred_host.cpp - a deferred renderer built like
# Spider-Man 2's frame - under Wine + Xvfb + lavapipe (software Vulkan).
#
#   SM2MARIO_TEST_ROM=/path/to/sm64.us.z64 D3DCOMPILER=/path/to/d3dcompiler_47.dll \
#     tests/run_e2e.sh <work-dir>
#
# D3DCOMPILER must be Microsoft's d3dcompiler_47.dll (Wine's can't compile the
# mock's shaders). The ROM is copied into <work-dir> only - never into the
# source tree or a package.
set -euo pipefail
cd "$(dirname "$0")/.."
WORK="${1:?work dir}"
ROM="${SM2MARIO_TEST_ROM:?set SM2MARIO_TEST_ROM}"
DXC="${D3DCOMPILER:?set D3DCOMPILER to Microsoft d3dcompiler_47.dll}"
SM64DLL="${SM64_DLL:-build-sm64/libsm64/dist/sm64.dll}"
SECONDS_TO_RUN="${E2E_SECONDS:-64}"

cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-mingw >/dev/null

GAME="$WORK/game"
rm -rf "$GAME" && mkdir -p "$GAME/scripts/resources/sm2mario" "$GAME/sm2mario"
x86_64-w64-mingw32-g++-posix -std=c++17 -O1 -static -DMOCK_PHYSICS tests/deferred_host.cpp tests/mock_game.S \
  -o "$GAME/DeferredGame.exe" -ld3d12 -ldxgi -ld3dcompiler
# A stand-in for the ModSettings mod (E2E_NO_MODSETTINGS=1: without it)
if [ -z "${E2E_NO_MODSETTINGS:-}" ]; then
  x86_64-w64-mingw32-g++-posix -std=c++17 -O1 -shared -static tests/mock_modsettings.cpp tests/mock_modsettings.def \
    -o "$GAME/scripts/ModSettings.dll"
fi
cp "$DXC" "$GAME/d3dcompiler_47.dll"
cp "$SM64DLL" "$GAME/scripts/resources/sm2mario/sm64.dll"
# (MOD_DLL: another build of the mod - an older release, to compare)
cp "${MOD_DLL:-build-mingw/sm2mario.dll}" "$GAME/scripts/sm2mario.dll"
cp package/resources/sm2mario/*.ini package/resources/sm2mario/*.txt "$GAME/scripts/resources/sm2mario/"
echo sm2mario.dll > "$GAME/scripts.txt"
cp "$ROM" "$GAME/sm2mario/sm64.us.z64"
chmod 600 "$GAME/sm2mario/sm64.us.z64"
# (A 0.4 user's file: settings chosen in it stay, see MigrateUserIni.)
printf "[General]\nLogLevel = info\nSettingsVersion = 0.4.0\n[Debug]\nShowOverlay = true\nTraceCollision = ${E2E_TRACE:-false}\n" > "$GAME/sm2mario/sm2mario.ini"
# (The stand-in shows Spider-Man for fewer frames before M than a player would:
# the stencil census decides after 40 frames of each instead of 150.)
printf "[Render]\nStencilCensusFrames = ${E2E_STENCIL_FRAMES:-40}\n" >> "$GAME/sm2mario/sm2mario.ini"
# The camera at the game's own distance (the checks' geometry), or E2E_CAMERA_DISTANCE times it.
printf "[Camera]\nDistance = ${E2E_CAMERA_DISTANCE:-1.0}\n" >> "$GAME/sm2mario/sm2mario.ini"
# E2E_COLLISION=world|flat: Mario's collision from the rendered world (0.3's) instead of the game's physics
if [ -n "${E2E_COLLISION:-}" ]; then printf "[Collision]\nSource = ${E2E_COLLISION}\n" >> "$GAME/sm2mario/sm2mario.ini"; fi
# E2E_INI_EXTRA: more settings for the user's sm2mario.ini (printf format), e.g. "[Cheats]\nMetalCap = true\n"
if [ -n "${E2E_INI_EXTRA:-}" ]; then printf "$E2E_INI_EXTRA" >> "$GAME/sm2mario/sm2mario.ini"; fi
# E2E_NO_PIN=1: without the hero pin hooks (the camera must then fail to follow Mario's jump)
if [ -n "${E2E_NO_PIN:-}" ]; then
  printf "[Hero]\nHoldDuringFrame = false\n[Camera]\nFollowMarioHeight = false\n" >> "$GAME/sm2mario/sm2mario.ini"
fi

export WINEDEBUG=-all WINEPREFIX="$WORK/prefix" VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export WINEDLLOVERRIDES="d3dcompiler_47=n"
export DISPLAY=:97
pgrep -f "Xvfb :97" >/dev/null || (Xvfb :97 -screen 0 960x540x24 >/dev/null 2>&1 &)
sleep 1
cd "$GAME"
rm -f shoot_*
(timeout $((SECONDS_TO_RUN + 60)) wine DeferredGame.exe "$SECONDS_TO_RUN" --script > host.out 2>&1 &)
running() { pgrep -f DeferredGame.exe >/dev/null; }
sleep 3
for n in 1 2 3 4 5 6 7; do
  for i in $(seq 1 240); do sleep 0.5; [ -f "shoot_$n" ] && break; running || break; done
  if [ -f "shoot_$n" ]; then
    xwd -root -display :97 -out shot.xwd && convert shot.xwd "$WORK/e2e-$n.png" && rm -f shot.xwd "shoot_$n"
  fi
done
for i in $(seq 1 240); do sleep 0.5; grep -q "host: done" host.out 2>/dev/null && break; running || break; done
echo "=== host"; grep "host:" host.out || true
echo "=== log"; cat sm2mario/sm2mario.log
echo "=== checks"
cd - >/dev/null
python3 tests/check_e2e.py "$GAME"
