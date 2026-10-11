# LSDj saving to flash and custom kits (plan B)

Date: 2026-10-12. Spec: `docs/superpowers/specs/2026-10-11-lsdj-transfer-save-design.md` (its "Part 2", and the page's
kit list and flash-use bar). Executed inline by the session that wrote it, on the owner's "go to the next part", so
this is the short form: decisions, units, tests. Branch `lsdj-xfer`.

## Changes to the spec's Part 2 (decided here)

- **Save key: REC held 1 s** (the spec said SEQ; SEQ became SELECT in the owner's new controls).
- **Flash commands are queued, not run by the console.** The console runs in the main loop, which is reachable from
  boot by direct calls; gate 4 now also forbids any flash write there. `save`, `kc`, `kx` and `kits` reply `ok` and
  set a request that the Game Boy's step (reached only through its pointer) carries out; `st` gives the result.
- **Kit banks are named by their flash index** on the wire (`k J`, `kx J`); `kb J` gives the ROM bank number the
  emulator shows it at (the J-th free kit bank of the ROM: 34 + J in 9.4.2).
- **Flash is written through the driver's RAM routines** (`fl_erase4k_ram`, `fl_prog_ram`), called from new
  wrappers in `core_ble_api.c` as `storage_hw.c` calls them; `storage.c`, `storage_hw.c` and `fm1_flash.h` are not
  edited and no `st_*` / `fl_*` function is added (gate 2b). The wrappers accept 0xA0000..0xDBFFF only.
- The page saves to flash after every change it makes (upload, delete, restore), so what it did survives power-off.

## Units

1. `core_ble_api.c` / `ble_core.h`: `core_flash_ptr(off)`, `core_flash_erase(off)`, `core_flash_prog(off, p, n)`.
2. `firmware/src/lsdj/lsdj_store.c` (pure, over three function pointers): the layout (K kit banks from the top, two
   slots of 30 - 2K sectors), the 32-byte record, PackBits packing, scan / load / save / change K / kit write and
   erase. Test `tests/lsdj/lsdj_store_test.c` on a simulated NOR flash: a power cut at every flash operation of a
   save leaves the old or the new save; first save erases Hortator's USR2 / USR3 headers; a save too big is
   refused and nothing is written; a damaged newest slot falls back; K found when slot A is damaged.
3. `lsdj_xfer.c`: `info` v2 (kits, slot capacity, used, saved), `kb`, `k`, `kd`, `kc`, `kx`, `kits`, `save`, `st`.
4. `lsdj.c`: load at PLAY, the bank table in RAM with the flash kit banks, the queued requests, REC held 1 s, the
   status line (`SAVING`, `SAVED nnK`, `SAVE FULL`, `SAVE FAILED`). Unit test with a simulated flash behind the
   core's wrappers: save, restart, the save is back.
5. `tools/check_ble_boot.py`: the flash wrappers are not reachable from boot except through `ble_key` / pointers.
6. `web/lsdjlink.js`, `web/lsdj.html`: kits travel with a song (a free flash kit bank, or one more after a `.sav`
   backup), the kit list with remove, "save now", the flash-use line; every change is followed by `save`.
7. `docs/lsdj/DEVICE_STEPS_SAVE.md`.

## Hard rules (unchanged)

Gate 2b's reference is never rewritten; nothing new runs at boot; flash writes only inside 0xA0000..0xDBFFF, only
on REC or a console command; the private build stays private.
