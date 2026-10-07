#!/usr/bin/env bash
# Regenerates src/render/inject_shaders_bin.h from src/render/inject_shaders.h
# with Microsoft's d3dcompiler_47.dll, run under Wine (Linux/WSL).
#   tools/compile_shaders.sh /path/to/d3dcompiler_47.dll
# (Any Windows install has one in System32; Electron's Windows zips ship one.)
set -euo pipefail
cd "$(dirname "$0")/.."
DLL="${1:?path to Microsoft d3dcompiler_47.dll}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
x86_64-w64-mingw32-g++-posix -std=c++17 -O1 -static tools/compile_shaders.cpp -o "$WORK/compile_shaders.exe"
cp "$DLL" "$WORK/d3dcompiler_47.dll"
( cd "$WORK" && WINEDEBUG=-all WINEDLLOVERRIDES="d3dcompiler_47=n" wine compile_shaders.exe out.h )
cp "$WORK/out.h" src/render/inject_shaders_bin.h
echo "wrote src/render/inject_shaders_bin.h"
