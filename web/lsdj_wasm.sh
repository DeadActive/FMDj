#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
# The install page's WebAssembly: web/lsdj_wasm.c -> build/web/lsdj_emu.wasm, with Emscripten: `emcc` if it is
# installed, else the emscripten/emsdk image in Docker (EMSDK_IMAGE names another tag). Nothing of LSDj is in it.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/web
ARGS="-O2 -w -Ifirmware/src -Ifirmware/src/lsdj --no-entry -sSTANDALONE_WASM -sFILESYSTEM=0 -sINITIAL_MEMORY=16777216 -sSTACK_SIZE=262144 -o build/web/lsdj_emu.wasm web/lsdj_wasm.c"
if command -v emcc >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    emcc $ARGS
else
    command -v docker >/dev/null 2>&1 || { echo "lsdj_wasm: no emcc and no docker"; exit 1; }
    IMG=${EMSDK_IMAGE:-$(docker images --format '{{.Repository}}:{{.Tag}}' emscripten/emsdk | head -1)}
    [ -n "$IMG" ] || IMG=emscripten/emsdk:latest
    # shellcheck disable=SC2086
    docker run --rm -v "$PWD":/src -w /src "$IMG" emcc $ARGS
fi
echo "lsdj_wasm: build/web/lsdj_emu.wasm ($(wc -c < build/web/lsdj_emu.wasm | tr -d ' ') B)"
