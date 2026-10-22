# FMDj roadmap

FMDj runs Game Boy music programs on the M-VAVE FM-1: the owner's own ROM in the firmware's Game Boy emulator
(Peanut-GB, minigb_apu), installed from the browser. This page says what is done, what comes next, and what is known
to be missing. Updated 2026-10-22.

## Done

- **LSDj** on the FM-1: the screen at 1.5x, the knobs and buttons as the Game Boy's keys, LSDj's screens on the
  PRESETS knob and OCT- / OCT+, the sound timed within the frame (no uneven beat), a second frame when the sound runs
  low (no stutters at 72-81 % CPU).
- **DMGo!** (Audiowanderer's drum machine), one program at a time: the 16 white keys are its steps, A and B on D#5 and
  F#5. A save in flash says whose it is; the other program's is kept until a save replaces it.
- **Install from the browser** (Chrome, Edge): your own ROM into the FMDj firmware, checked in the emulator first,
  installed over USB. Nothing of LSDj or DMGo is in the firmware or on the site.
- **Saves in flash**: REC held for a second; PLAY loads it. LSDj's custom kits in flash too.
- **The song page**: LSDj's songs and kits in and out over USB; DMGo's whole save.
- **The simulator**: the FM-1 and its firmware in the browser, with your ROM, saves and projects.
- **The GLO menu**: screen themes (DMG green, Pocket, Light, Super Game Boy, Dark), the status bar (FPS, CPU, gaps;
  off at first, the picture then centred), the emulator's options; kept in flash.

## Next

1. **The link port.** The Game Boy's serial port in the emulator (Peanut-GB has the hooks: `gb_serial_tx` /
   `gb_serial_rx`), clocked from the FM-1 side. Both steps below stand on it.
2. **Sync (MIDI clock).** The FM-1's MIDI clock in and out driving the programs' own sync: LSDj's sync modes over the
   link port, DMGo's LINK IN / OUT and PULSE OUT. FMDj then plays in time with other gear, and leads it.
3. **mGB (MIDI synth).** A third program (trash80's mGB, the owner's own copy as with the others): the FM-1's keys
   and MIDI play the Game Boy's four channels as a chip synth, sent to it over the link port as an Arduinoboy would.

## Known gaps

- The status bar shows `FPS 0.0` for its first 4 seconds after PLAY, and FPS wobbles by 0.2 at real time.
- In the GLO menu the PRESETS knob and OCT- / OCT+ also act (they send SELECT with a direction).
- With LSDj's kit banks at 11 or 12, a DMGo save that packs poorly may not fit (SAVE FULL); the kit banks can only be
  given back from LSDj.
- `ble kits` with DMGo's save kept answers as a flash failure, not with its own reason.
- The simulator keeps LSDj's panel when the browser blocks storage and the ROM changes to DMGo.
- A DMGo of another version gets the step labels in the simulator, though the firmware gives it buttons only.

## Waiting for a check on the device

- DMGo installed with the install page; its steps from the white keys.
- The GLO menu: themes, the status bar, the centred picture, the settings kept over a power-off
  (`docs/lsdj/DEVICE_STEPS_MENU.md`).
