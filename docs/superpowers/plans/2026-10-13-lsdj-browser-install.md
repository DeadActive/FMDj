# LSDj browser install: plan (short form)

Spec: `docs/superpowers/specs/2026-10-13-lsdj-browser-install-design.md`. Inline, test first, one review at the end.

1. **The cartridge block** `firmware/src/lsdj/lsdj_cart.c` (pure): `lsdj_cart_rom_check`, `lsdj_cart_need`,
   `lsdj_cart_make`, `lsdj_cart_read`. Block = body + 16-byte trailer at the very end of the app's flash slot:
   `"LSDC"`, version, banks, body length, CRC-32 of the body. Body: bank lengths (u16 each), packed save length,
   packed save, bank bytes. Test: `tests/lsdj/lsdj_cart_test.c`.
2. **The firmware without a ROM**: `lsdj.c` reads the block on PLAY (`core_cart_end()`), no `lsdj_rom.h`; no block:
   `NO LSDJ ROM`. `build.py`: `FELUCCA_LSDJ=1` alone builds `lsdj-base.fwsc`; with `LSDJ_ROM` it appends the block
   (`tools/lsdj_cart_tool.c`, the same C) and writes `lsdj-private.fwsc`. The unit test runs without a ROM for the
   no-block cases and with `build/lsdj/cart.bin` for the rest. Gates as before.
3. **WebAssembly**: `web/lsdj_wasm.c` (the emulator, the builder, a frame at a time) built by `web/lsdj_wasm.sh`
   (Emscripten in Docker) to `build/web/lsdj_emu.wasm`.
4. **The page's logic** `web/lsdjinstall.js`: the three runs as `tools/lsdj_rom.py` does them, the block, its place
   in the base package, `Package.setApp`, a re-parse. Tests in node: our test ROM; with the owner's ROM the block
   and the whole package are byte for byte the Mac build's.
5. **The page** `web/lsdj_install.html`, `make_site.py` (base package, wasm, the demo's key list), a check sheet,
   `LICENSING.md`. Whole suite, both builds, one review.
