# LSDj on the FM-1: song transfer and saving to flash: design

Date: 2026-10-11. Author: DEADACTIVE (with Claude). Branch: `lsdj-xfer` (from `lsdj-emu` 5d45704).

## Goal

Today the LSDj build keeps LSDj's 128 KiB save only in RAM: power-off loses everything, and there is no way to
move a song on or off the device. This work adds:

1. **Transfer.** A page in Chrome that uploads `.lsdprj` / `.lsdsng` songs to the FM-1 and downloads songs from it
   as `.lsdprj`, plus the whole save as `.sav`. Custom kits travel with the songs.
2. **Saving on the FM-1.** The save and the custom kits are kept in flash and survive power-off.

Order: transfer first (it is also the backup route), then saving. Two plans:

- **Plan A, transfer**: the console command without `k*`, `kits` and `save`; the page with songs and `.sav`. A
  song whose kits are all in the ROM uploads; one with other kits is refused with their count until plan B.
- **Plan B, saving**: flash, the save key, custom kits, the flash-use bar.

Not in this version:

- automatic saving, or an "unsaved" mark on the screen (saving is a key press);
- downloading the song being edited without saving it in LSDj first: LSDj changes the song when it saves it to
  a slot (measured on 9.4.2: the 4 KiB of waves are stored bit-inverted, and two bookkeeping bytes differ), and
  that step belongs to each LSDj version. Save in LSDj (PROJECT, LOAD/SAVE SONG, SAVE), then download; the
  `.sav` download holds the edited song as it is;
- a command-line tool (the page is the only host tool; its logic is one JavaScript module with node tests);
- editing kits, fonts or palettes (LSDPatcher does that on the ROM file);
- any change to the ROM inside the firmware, to the CPU clock or to the emulator's speed options.

## User decisions (2026-10-11)

- The save lives in the **user-sample area** of Felucca's store only (option B): Hortator's projects, presets and
  settings stay; its user samples USR1..USR3 are lost.
- The host tool is a **Chrome page, in this step**.
- Saving is an explicit key press; custom kits are kept in flash.

Defaults chosen by Claude, to confirm at review:

- the save key is **SEQ held 1 s** (ENV and SEQ are the only free panel buttons);
- the layout below (kit banks at the top of the area, two save slots sharing the rest).

## Hard rules

- **The updater stays reachable.** The update path's machine code is unchanged: `tests/ble/update_path.ref` is not
  rewritten by this work, and gate 2b passes against it in every build. If a new flash caller changes how the
  compiler builds a compared function, the caller is rewritten until the gate passes; the reference is never moved.
- **Nothing new runs at boot.** Flash is read when PLAY starts LSDj, and written only on the save key or a console
  command. Gate 4 (`tools/check_ble_boot.py --lsdj`) is extended: no flash write is reachable from boot.
- **Flash writes stay inside 0xA0000..0xDBFFF.** The core's new write wrappers refuse every other address.
- **Private build.** Nothing made from the ROM is committed or published. The page and its modules hold no LSDj
  data and are committed.

## Background (facts this design rests on)

- **`.lsdprj`** (LSDPatcher, MIT; read from `LSDSavFile.java`): 8 name bytes, 1 version byte, the song's compressed
  512-byte blocks in order, then one raw 16 KiB ROM bank per kit the song uses, in ascending kit order. A `.lsdsng`
  is the same without the kit banks.
- **The save** (128 KiB): the working song at 0x0000..0x7FFF; the file system from 0x8000: 32 names (8 bytes each),
  32 versions at 0x8100, the active slot at 0x8140, the block table at 0x8141 (191 blocks), blocks from 0x8200. The
  start-up save of the build has an empty, initialised file system.
- **Compression**: 0xC0 and 0xE0 escapes (RLE, default wave, default instrument, block switch `E0 nn`, end `E0 FF`).
- **Kit numbers**: a kit instrument (type 2) names its kits at instrument bytes 2 and 9, low 6 bits; kit k is ROM
  bank k + 8, or k + 13 above bank 26. LSDj 9.4.2 has 21 kits (banks 8..26, 32, 33) and 30 free banks (34..63); a
  free bank starts 0xFF 0xFF.
- **Flash**: the driver writes only inside Felucca's store. The user-sample area is 0xA0000..0xDBFFF: 240 KiB, 60
  sectors of 4 KiB. The core maps flash above 0x93000 as plain memory, so data there is read through a pointer.
- **Hortator** takes a user-sample slot only with its `FSMP` header at 0xA0000, 0xB4000 or 0xC8000.
- **The console** (USB serial, VID 0x1209 / PID 0x0001): its handler is compiled into `fm1_main`, which gate 2b
  compares, so `console.c` cannot change at all (measured 2026-10-12: a new word or a longer line fails the gate).
  Its existing word `ble` already calls the unit (`ble_status`). Its line is 64 characters.
- **This build**: image 562,632 B of 585,728; pool 295,988 B of 344,064; `.data` + `.bss` 14,816 B of 98,304. The
  low-level erase and program routines (`fl_erase4k_ram`, `fl_prog_ram`, RAM code) are in the build; Felucca's
  `st_erase` / `st_prog` are not.

## Part 1: the device side of transfer

### Console command

The command word on the wire is the console's existing `ble`: `ble WORDS` reaches the unit's `ble_status`, which
reads the line through one new core accessor (`core_con_line`) and takes the words as a transfer command; a bare
`ble` prints the status as before. `console.c` is not edited. The tables below write `lsdj` for these words.
Every data line carries a CRC-16 (XMODEM) of its bytes; the host sends one line and waits for the reply.

| Command | Does |
|---|---|
| `lsdj info` | protocol version, started / held, kit banks (ROM bank numbers, which hold a kit), flash layout and use |
| `lsdj hold` | stops the Game Boy between two frames; audio silent; refused until PLAY has started LSDj |
| `lsdj go` / `lsdj go reset` | runs again; `reset` restarts LSDj so it reads the changed file system |
| `lsdj r ADDR LEN` | save RAM as hex, up to 256 bytes a reply |
| `lsdj w ADDR HEX CRC` | writes save RAM, up to 20 bytes a line (the 64-character console line) |
| `lsdj b BANK OFF LEN` | ROM bank bytes through the bank table (flash kit banks included), up to 256 a reply |
| `lsdj bc BANK` | the CRC-32 of the whole 16 KiB bank: the page finds a song's kits on the device by it |
| `lsdj k BANK` + `lsdj kd OFF HEX CRC` + `lsdj kc CRC32` | a kit bank: into a 16 KiB RAM buffer, then written to flash and read back |
| `lsdj kits K` | changes the number of flash kit banks (part 2) |
| `lsdj save` | saves to flash now (part 2) |

Numbers are hex. A reply is one line: `ok ...` or `err REASON`. `go reset` is carried out by the Game Boy's own
step in the main loop, not by the console (gate 4).

`r`, `w`, `k*`, `kits` and `save` need `hold`. While held, the status line shows `USB`. If the host goes away, the
unit runs again after 5 s without a command.

### Pure code

`firmware/src/lsdj/lsdj_xfer.c`: the command parser and replies, no hardware; host-tested with a fake save RAM.

## Part 2: saving to flash

### Layout of 0xA0000..0xDBFFF (60 sectors)

- **Kit banks**: K of them (0..12), 16 KiB each, raw, from the top down. Flash kit bank j stands for the j-th free
  bank of the ROM (bank 34 + j in 9.4.2): the emulator's bank table is copied to RAM at start and those entries
  point into flash. An erased bank reads 0xFF, which LSDj takes as free.
- **Save slots A and B**: S = 30 - 2K sectors each; A at sector 0, B at sector S.
- **A slot**: a 32-byte record (magic `LSDS`, layout version, K, sequence number, packed length, CRC-32 of the
  packed bytes, CRC-32 of the record) and the save, PackBits-packed as the start-up save is.

With no kit banks a slot holds 120 KiB; each kit bank takes 8 KiB from each slot. A completely full LSDj file
system packs to about 100 KiB, so it leaves room for 2 kit banks; a typical save leaves room for many.

### Saving (SEQ held 1 s, or `lsdj save`)

1. The Game Boy stops between frames; the audio ring is emptied.
2. A counting pass gives the packed size. Too big for a slot: `SAVE FULL`, nothing is written.
3. The slot that does not hold the newest save is erased (only the sectors needed) and programmed: the packed bytes
   first, read back and checked, the record last. A power cut at any point leaves the other slot as it was.
4. The status line shows `SAVED` and the size; the Game Boy runs again.

On the first save (no slot with a good record yet, so K = 0) the sectors at 0xB4000 and 0xC8000 are erased
before the slot is written, so Hortator sees USR2 and USR3 as empty and not as damaged samples.

### Loading (when PLAY starts LSDj)

The newest slot with a good record and a good CRC is unpacked into the save RAM. K comes from slot A's record; if A
is damaged, slot B is searched at each possible S. No good slot: the start-up save of the firmware, as today.
Nothing is written.

### Changing K (`lsdj kits K`, sent by the page only)

Both slots move when K changes, so this is the one step that is not power-cut safe. The page first downloads the
`.sav` to the Mac. The unit then checks that the packed save fits the new slot size, erases both records and the
newly taken kit banks, and writes the save into slot A of the new layout.

### Flash access

Three new functions in `core_ble_api.c`: a pointer into plain flash, erase one sector, program a run of bytes. The
two writing ones accept only 0xA0000..0xDBFFF, silence the audio buffer, and call the RAM routines as the updater
does. `firmware/src/lsdj/lsdj_store.c` holds the layout, packing and slot choice as pure code over three function
pointers (read, erase, program).

## Part 3: the Chrome page

- `web/lsdjfs.js`: the formats, no browser: the save's file system (list, add, remove a song), `.lsdprj` /
  `.lsdsng` in and out, unpacking, kit renumbering as LSDPatcher does (an identical bank already on the device,
  found by CRC-32, is reused; a missing one takes a free flash kit bank).
- `web/lsdjlink.js`: the console protocol over a byte stream (Web Serial in the page, a simulated device in tests).
- `web/lsdj.html`: connect; the song list (slot, name, version, blocks) with a download button each, drop files to upload, the kit list with remove buttons, `.sav` download and upload, and a
  bar of flash use. `web/make_site.py` adds it as `webapp/lsdj/index.html`, self-contained like the installer.

An upload: hold, read the file system, add the song and its kits, write only the changed bytes, save to flash,
`go reset`. A song that needs more kit banks than are free: the page offers to raise K if the save still fits, and
says what to remove if not.

## Errors

- A bad CRC or a lost line: the page repeats it three times, then stops and sends `lsdj go`; the save RAM is only
  changed in whole verified lines, and the file system's block table is written last.
- A damaged file (no `E0 FF` end, kit data not a multiple of 16 KiB, no free slot or blocks): refused on the Mac,
  before anything is sent.
- A flash write that does not read back: the slot's record is not written; `SAVE FAILED` on the status line.

## Testing

- **Host, C**: `lsdj_store` on a simulated flash, with a power cut after every erase and program step: the load
  always gives the old or the new save. `lsdj_xfer` commands against a fake save RAM.
- **Host, node**: `lsdjfs` round trips; the page's flows against a simulated device written in JavaScript; a song
  exported, imported into another save and unpacked to the same 32 KiB. The C test of `lsdj_xfer` writes its
  command and reply lines to a file, and the node test replays them against the simulated device, so the two
  sides cannot drift apart.
- **Host, the real ROM** (private, the existing harness): LSDj saves the demo song to a slot; `lsdjfs` exports it
  and adds it to another save in other blocks; LSDj's own file screen loads it; the working memory equals LSDj's
  own load outside its timer bytes (0x3FB0..0x3FCF) and the song plays. (Tried by hand on 2026-10-11: one byte,
  0x3FB8, differed.)
- **Gates**: 2a, 2b (reference untouched), 4 (extended), image and pool sizes.
- **Device** (the owner): upload a `.lsdprj` with a custom kit, load and play it; save; power off and on; the song
  and kit are there; download it and compare with the file; `.sav` round trip; the update path still installs.

## Open points measured during the work

- The time of a save (expected about 50 ms a sector) and of a full `.sav` transfer.
- Whether a second caller changes any compared function (gate 2b decides; see the hard rules).
