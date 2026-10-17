# FMDj: DMGo support

## Goal

FMDj runs a second Game Boy program, **DMGo!** (Drum Machina Go!, Audiowanderer, free download from
https://audiowanderer.com/AW/drum-machina-go-or/), besides LSDj. The owner brings their own DMGo ROM the same way as
LSDj: nothing of DMGo is in the firmware, the repo or the published site.

## Decisions (the owner's)

- **One program at a time.** The install tool installs whichever ROM it is given (LSDj or DMGo); switching means
  installing the other one.
- **Keys as the 16 steps.** In DMGo the 16 white keys F3..G5 toggle steps 1..16 of the channel page shown. A and B
  move to the black keys D#5 (A) and F#5 (B). LSDj keeps its mapping (A = F5, B = G5).
- **Pages:** the install tool, the simulator, and a whole-save backup on the song page. The landing page does not
  change.
- **Build:** native execution, one final review.

## Facts measured (DMGo v1, `DMGo_v1.gb`)

- 64 KiB (4 banks), original Game Boy only (0x143 = 0x00), title "DRUMMACHINA", cartridge type 0x1E (MBC5 + rumble +
  RAM + battery), RAM code 0x03 (32 KiB), header checksum 0x9B, global checksum 0xEE3C.
- It runs in our Peanut-GB with no emulator errors at ~85 us a frame (LSDj needs far more).
- Peanut-GB's `cart_ram[]` says 0x1D and 0x1E have no RAM: wrong (both have RAM); without the fix DMGo cannot save.
- DMGo's WRAM (found with the host emulator, a step toggled with B and the memory diffed):
  - `C005` page: 0..3 = CH1..CH4, 4 = PATTERNS (5, 6 = save, setup).
  - `C006` = 1 while the top menu (A) is open; `CA34` = 1 while PICK A PAT is open; `C02F` = 0 on the title screen.
  - `C01A` cursor field (0 = BPM, 12 = the SEQ row); `C01B` and `CB1F` the cursor's step column (0..15).
  - `C02C` the pattern slot being edited; `C04D + 2*ch` its 16-bit step mask (bit 0 = step 1);
    `C0C1`, `C801`, `C875` (+ 2*ch) the playing slot's copies.
- **The step macro** works and is exact: for one frame set the cursor to field 12 / column N-1 and press B, the next
  frame put the cursor back. The result equals a real B press on that step (same memory, same sound: audio peak 4298),
  also in a pattern slot that is not playing, and the cursor is where it was. No DMGo routine is re-implemented: DMGo
  does its own toggle.
- DMGo's LINK IN/OUT and PULSE OUT sync use the link port, which the emulator does not have: DMGo runs on its internal
  clock. Out of scope.

## Design

### Program profile (firmware, `firmware/src/lsdj/`)

A small table of programs, picked once from the installed ROM's header:

| | LSDj | DMGo |
|---|---|---|
| accepted when | type 0x1B, RAM 0x04 (as now) | title "DRUMMACHINA", type 0x1E, RAM 0x03 |
| save size | 128 KiB | 32 KiB |
| A / B | F5 / G5 | D#5 / F#5 |
| PRESETS, OCT-/+ screen shortcuts | on | off |
| step keys | off | on, only for global checksum 0xEE3C (v1); another version gets buttons only |
| splash lines | as now | DMGo's own (PLAY: START, A: D#5 B: F#5, STEPS: WHITE KEYS) |

Anything else is refused as now ("NOT LSDJ OR DMGO"). The knobs (SELECT = LEFT/RIGHT, ALGORITHM = UP/DOWN), the
buttons (HOME/ARP/SCL/SAVE, SEQ = SELECT, PLAY = START), REC held 1 s to save, the four comparison switches and the
status line work the same for both.

### Cartridge block and emulator

- The block format does not change. The save size comes from the ROM's own header (0x149: 0x04 = 128 KiB,
  0x03 = 32 KiB), so existing LSDj blocks read exactly as before.
- `lsdj_cart_rom_check` and `emu_init` accept the two profiles. The emulator's save RAM bound is the profile's size
  (the static buffer stays 128 KiB).
- Peanut-GB (vendored): `cart_ram[0x1D] = cart_ram[0x1E] = 1`; for the rumble types (0x1C..0x1E) the RAM-bank
  register's bit 3 (the motor) is ignored.
- LSDj's wait-loop finder already finds nothing in other ROMs and leaves them alone.

### Step keys (DMGo v1 only)

- A white key press (edge) queues its step; up to 16 wait. A queued step runs only when DMGo is on a channel page
  with nothing over it: `C02F != 0`, `C005 <= 3`, `C006 == 0`, `CA34 == 0`; otherwise it is dropped.
- One step takes two frames: frame 1 saves `C01A`, `C01B`, `CB1F`, sets them to 12 / N-1 / N-1 and holds B (whatever
  the F#5 key is doing); frame 2 restores them with B up. If the owner holds B (F#5) when a step is due, it waits
  until B is released.
- Pure logic (queue, guard, the two frames) lives in `lsdj_gb.c` for unit tests; `lsdj.c` applies it around
  `emu_frame`.

### Saving to flash

- The store keeps one save. Its record gains the program (LSDj or DMGo, a store version bump; a version 2 record is
  LSDj's). At start the flash save is loaded only if it is the installed program's; otherwise the cart's start save
  is used and the status line says so ("SAVE IS LSDJ'S: REC REPLACES IT"). The other program's save stays in flash
  until REC saves over it.
- LSDj's kit banks are LSDj's only; DMGo maps no kit banks.

### Install tool (`web/lsdj_install.html`, `web/lsdjinstall.js`, `web/lsdj_wasm.c`)

- Text: "your LSDj or DMGo ROM", with a link to DMGo's download page.
- `makeCart` takes the profile from the ROM: for DMGo the start save is the 32 KiB save RAM after 600 frames (no demo
  song). The check that it runs: START past the title, one step set (cursor to SEQ, B), START plays, sound peak at
  least 2000; then the start save is the one from before the check.
- Before installing over another program, the page says that the FM-1's saved songs of the other program stay until
  a save replaces them, and points to the song page to back them up.

### Simulator (`web/sim/`)

- Loads a DMGo ROM and an optional `.sav` (32 KiB) like LSDj's. The firmware's own key map applies (the sim compiles
  `lsdj.c`); the computer-keyboard X / Z press the program's A / B keys.
- LSDj-specific text names the program loaded.

### Song page: whole-save backup (`web/lsdj.html`, `web/lsdjlink.js`)

- The console word `info` reports the program and the save size. With DMGo installed the page shows only "Download
  the save" and "Upload a save" (exactly 32768 bytes); the LSDj song list, kits and project files are hidden.
- `uploadSav` / `downloadSav` take the size from `info`; the LSDj "jk" check stays for LSDj only.

## Constraints

- The update path is unchanged (gate 2b), and gate 4 (`check_ble_boot.py --lsdj`) passes with any new function
  added to its lists. The updater stays reachable.
- Nothing from the DMGo ROM is committed or published: no ROM bytes, screenshots or saves. Memory addresses and the
  two header checksums are facts about it, not its content.
- Tests that need the ROM read it from `DMGO_ROM` and are skipped without it, like `LSDJ_ROM`.
- No clock changes. Credit: DMGo! by Audiowanderer, named on the install page and in the sim.

## Testing

- Unit (no ROM): profile selection from synthetic headers (LSDj, DMGo, DMGo with another checksum, other ROMs); the
  step queue and guard; the 0x1E RAM fix; the store's program field (an LSDj save is not loaded for DMGo).
- Host (`DMGO_ROM`): a step key gives the same memory as a real B press on that step, in slot 1 and in slot 2, and
  playback sounds; the cursor is unchanged; a key on the title, menu or PICK A PAT does nothing; LSDj's results are
  unchanged.
- The install, sim and song-page JS tests cover DMGo's path with `DMGO_ROM`.
- By eye: the simulator with DMGo (steps from the keys), then the device after install.
