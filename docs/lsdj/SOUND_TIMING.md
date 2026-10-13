# LSDj on the FM-1: sound timing and speed (2026-10-13)

The owner heard micro stutters. Two causes were found on the host with LSDj 9.4.2, and both are fixed here.

## 1. The sound was made once per Game Boy frame

LSDj's sound engine runs on the timer interrupt, about 6 times in a Game Boy frame (358 times a second), and writes
the sound registers each time. The sound chip emulation (minigb_apu) made a whole frame's sound in one go after the
frame, so every write of a frame was heard from the frame's start.

- A note began up to 16.7 ms early, by a different amount each time: an uneven beat.
- Several writes to one register within a frame (envelopes, tables, kits) collapsed into the last one.
- The sound changed in steps 60 times a second. In two full songs the jump of the waveform at a frame boundary
  was 3 times the jump anywhere else.

Now the sound is made piece by piece: before a sound register is written or read, the sound is made up to where in
its frame the Game Boy is (`lsdj_emu.c`, `EMU_SND_FINE`).

| | before | after |
|---|---|---|
| A note every 64 timer interrupts (the test program): gaps between notes | 7380 .. 8118 samples | 7871 .. 7873 samples |
| Waveform jump at frame boundaries against anywhere (a full song) | 1960 against 572 | 637 against 580 |

## 2. The FM-1 had almost no time to spare

`LOAD` was about 90 % with the demo song. Full songs make LSDj run up to 15 % more instructions per frame, which
puts single frames over 100 %; the sound buffer then runs dry and a gap is heard.

While a song plays, LSDj spins between its interrupts in a loop of 8 instructions that only reads two flags. That
loop was 55 % of all instructions with the demo and 27 .. 31 % with full songs. The emulator now moves the
clocks past the loop's instructions without running them, up to the next timer, screen or serial event
(`lsdj_emu.c`, `EMU_WAIT_SKIP`). Nothing else changes: after every frame the registers, counters, memory and sound
are the same as when the loop is run (`tests/lsdj/lsdj_emu_test.c`, `tests/lsdj/lsdj_modes_test.py`).

| Instructions run per frame while a song plays | before | after |
|---|---|---|
| The demo song | 7330 | 3590 |
| A full song | 7500 | 5250 |
| Another full song | 7420 | 5840 |

How much `LOAD` falls on the FM-1 is to be read from its status line (`docs/lsdj/DEVICE_STEPS_SOUND.md`).

## What is not changed

- Peanut-GB's CPU and screen timing, and minigb_apu's sound generation, are as before.
- When LSDj switches the screen off and on, Peanut-GB's frame is not a frame long, and sound timing around that
  moment is off by up to 0.5 ms.
- The sound still passes through a buffer of up to 93 ms (`RING_N` in `lsdj.c`): a key is heard that much later.

## If more speed is needed

- Peanut-GB updates its counters after every instruction. Doing that only when an event is due is the next step;
  the same tests would prove it changes nothing.
- The emulator's code runs from flash. Whether running it from RAM is faster is unmeasured.
