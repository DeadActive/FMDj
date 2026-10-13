#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
# The LSDj simulator's WebAssembly: web/sim/lsdjsim.c -> build/web/sim/lsdjsim.wasm, with Emscripten (`emcc`, or the
# emscripten/emsdk image in Docker). Needs one ./build.sh before it (build/gen: the fonts). Nothing of LSDj is in it.
set -e
cd "$(dirname "$0")/../.."
[ -f build/gen/ui_fonts.h ] || { echo "sim: run ./build.sh once first (build/gen)"; exit 1; }
mkdir -p build/web/sim
VERSION=$(sed -n 's/^#define FELUCCA_VERSION "\([^"]*\)".*/\1/p' firmware/src/felucca.c)
EXP=_sim_init,_sim_render,_sim_btn,_sim_key,_sim_enc,_sim_master,_sim_leds,_sim_key_leds,_sim_playing,_sim_fb,_sim_audio,_sim_flash,_sim_flash_dirty,_sim_flash_clean,_sim_flash_reset,_sim_cart,_sim_cart_max,_sim_cart_clear,_sim_frames,_sim_error,_sim_con_in,_sim_con
printf '#define FELUCCA_VERSION "%s"\n' "$VERSION" > build/web/sim/sim_version.h
ARGS="-O2 -w -include build/web/sim/sim_version.h -Ibuild/gen -Ifirmware/src -Ifirmware/src/lsdj --no-entry -sSTANDALONE_WASM -sFILESYSTEM=0 -sINITIAL_MEMORY=16777216 -sSTACK_SIZE=262144 -sEXPORTED_FUNCTIONS=$EXP -o build/web/sim/lsdjsim.wasm web/sim/lsdjsim.c"
if command -v emcc >/dev/null 2>&1; then
    eval "emcc $ARGS"
else
    command -v docker >/dev/null 2>&1 || { echo "sim: no emcc and no docker"; exit 1; }
    IMG=${EMSDK_IMAGE:-$(docker images --format '{{.Repository}}:{{.Tag}}' emscripten/emsdk | head -1)}
    [ -n "$IMG" ] || IMG=emscripten/emsdk:latest
    docker run --rm -v "$PWD":/src -w /src "$IMG" sh -c "emcc $ARGS"
fi
echo "sim: build/web/sim/lsdjsim.wasm ($(wc -c < build/web/sim/lsdjsim.wasm | tr -d ' ') B, $VERSION)"
