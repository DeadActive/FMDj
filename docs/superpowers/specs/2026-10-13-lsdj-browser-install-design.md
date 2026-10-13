# LSDj on the FM-1: the install page that takes your ROM (design, 2026-10-13)

## What the owner asked for

"The install tool, where you load your LSDj ROM, compile and install, all in the browser."

Today the ROM is compiled into the firmware on the Mac (`LSDJ_ROM=... ./build.sh`, Docker), so only the owner can
make an LSDj firmware, and that firmware may not be given to anyone. After this work:

- The firmware is built once **without any ROM**. It contains nothing of LSDj.
- A page takes the user's own ROM file, checks it, puts it into that firmware and installs the result on the
  FM-1. The ROM never leaves the browser.

## How

1. **The cartridge block.** The ROM is no longer C arrays in the firmware. It is one block of data placed after
   the firmware's own bytes inside the app's flash slot: a mark, the number of banks, each bank's length, the
   packed start-up save, the banks (each without its trailing 0xFF bytes, as now), a CRC-32. LSDj's unit reads it
   on PLAY. With no block, or a damaged one, PLAY shows `NO LSDJ ROM: USE THE INSTALL PAGE` and nothing else runs.
   Room: the slot is 581,564 B and the firmware about 102 KB, so about 479 KB; LSDj 9.4.2 needs 471 KB.

2. **One builder, in C.** `firmware/src/lsdj/lsdj_cart.c` (pure, host-tested) makes the block from a ROM and a
   save, and reads it back. The Mac build and the browser both use this same code, so they make the same bytes.

3. **The emulator in the browser.** `lsdj_emu.c` and the builder are compiled to WebAssembly (Emscripten in Docker,
   as the firmware uses Docker; the image is already on this Mac). The page runs the user's ROM with it exactly
   as the Mac build does now: LSDj's first start from an empty save, then the demo song typed in key by key, then
   a short play to see that it sounds. That takes some seconds, and it proves the ROM runs in our emulator before
   anything is written to the FM-1. The demo's key list is ours (`tools/lsdj_demo.py`) and ships as data.

4. **The package.** `web/fm1pkg.js` already redoes the app's checksums and the flash encryption for a new app
   image (`Package.setApp`, `serialize`). The page: base package + cartridge block -> package -> the existing
   installer over Web MIDI.

5. **The page**: `webapp/lsdj-install/`. Choose the ROM; the checks and the run with a progress line; then Install.
   Refusals say why: not an LSDj ROM (cartridge type, save size, header checksum), does not run, or too big and by
   how many bytes.

6. **The Mac build stays**: `LSDJ_ROM=... ./build.sh` makes the base, then appends the same block. Its package must
   be byte for byte what the page makes from the same ROM.

## Safety

The FM-1's loader writes the app without checking the app's own checksum, so a wrongly made package could leave
the FM-1 not starting (the way back is the UBOOT tool). Therefore:

- A test builds the package both ways, Mac and browser code (in node), from the same ROM and compares every byte.
  The Mac's package is the kind already proven on the device.
- Before sending, the page parses its own package again (all checksums) and checks the product name.
- The base firmware passes the same gates as now (update path unchanged, nothing at boot). The cartridge block is
  only read after PLAY.

## Tests

- The builder: round trip, damaged and cut-off blocks refused, sizes at the limit.
- The whole way without LSDj: our own small test ROM through builder, WebAssembly, package, and the firmware unit
  against a simulated core.
- With the owner's ROM (skipped without it): browser package = Mac package; LSDj starts and plays from it.

## Not in this work

- Publishing the page and the base firmware. It becomes possible; where and when is the owner's decision.
- ROMs that do not fit (many extra kits): refused. Kits go in through the song page, as now.
- Checking the page in Chrome with a real FM-1 is the owner's (a check sheet comes with it).
