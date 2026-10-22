# FMDj

[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)

Game Boy music programs inside the M-VAVE FM-1: **LSDj**, the tracker, and **DMGo!**, the drum machine. They run on
the FM-1's own firmware in a Game Boy emulator, on its screen, keys, knobs and sound. You bring your own ROM; the
install page puts it into the firmware in your browser and installs it over USB.

- **Site:** [deadactive.github.io/FMDj](https://deadactive.github.io/FMDj/): what it is, screenshots, how to use it
- **Install:** [install page](https://deadactive.github.io/FMDj/webapp/lsdj-install/) (Chrome or Edge, USB, your own
  LSDj or DMGo ROM)
- **Try first:** [simulator](https://deadactive.github.io/FMDj/webapp/lsdj-sim/): the FM-1 and this firmware in the
  browser, with your ROM
- **Songs and saves:** [song page](https://deadactive.github.io/FMDj/webapp/lsdj/) (Chrome, USB)

## Your own ROM

Nothing of LSDj or DMGo is in this repository, the firmware or the site's packages.

- **LSDj** is by Johan Kotlinski: [littlesounddj.com](https://www.littlesounddj.com/).
- **DMGo!** (Drum Machina Go!) is by Audiowanderer, free from
  [audiowanderer.com](https://audiowanderer.com/AW/drum-machina-go-or/).

One of them is on the FM-1 at a time; to change, install the other one.

## Getting started

1. Get your ROM (`.gb`).
2. If you like, try it in the [simulator](https://deadactive.github.io/FMDj/webapp/lsdj-sim/) first.
3. Open the [install page](https://deadactive.github.io/FMDj/webapp/lsdj-install/) in Chrome or Edge and choose
   your ROM: the page runs it in the emulator to check it. Connect the FM-1 by USB, directly to the computer, and
   press Install. Do not unplug it while writing.
4. On the FM-1, **PLAY** starts the program. In LSDj, PLAY again plays the demo song.

## Controls

| Game Boy | FM-1 |
|---|---|
| ◀ ▶ | SELECT knob; HOME / ARP |
| ▲ ▼ | ALGORITHM knob; SCL / SAVE |
| A / B | LSDj: the top F / G keys. DMGo: D#5 / F#5 |
| START / SELECT | PLAY / SEQ |
| LSDj's screens (SELECT + direction) | PRESETS knob; OCT+ / OCT- |
| DMGo's steps 1 to 16 | the 16 white keys |
| the menu | GLO |
| save to flash | REC held for a second |

**GLO** opens a menu over the lower half of the screen while the music plays on: the screen's colours (DMG green,
Pocket, Light, Super Game Boy, Dark), the status bar (FPS, CPU, gaps; off at first), and the emulator's options. It
is kept in flash. All of it: [docs/lsdj/CONTROLS.md](docs/lsdj/CONTROLS.md).

## The emulator

Peanut-GB (the processor, memory and screen) and minigb_apu (the four sound channels) run inside the firmware. The
screen is drawn at 1.5x, the sound is made as the program writes it within the frame (notes land on their beat), and
when the sound runs low a second Game Boy frame runs at once, so it does not stutter.

## Roadmap

Next: the Game Boy's link port in the emulator, then sync by the FM-1's MIDI clock (LSDj's sync, DMGo's LINK /
PULSE), then mGB as a third program. The whole list, with what is done and the known gaps:
[docs/ROADMAP.md](docs/ROADMAP.md).

## Building

```sh
FELUCCA_LSDJ=1 ./build.sh                       # build/lsdj-base.fwsc: the firmware without a ROM
FELUCCA_LSDJ=1 LSDJ_ROM=your.gb ./build.sh      # also build/lsdj-private.fwsc, your ROM in it: never publish it
sh tests/run_tests.sh                           # the tests (LSDJ_ROM=, DMGO_ROM= for those that need a ROM)
tools/fmdj_publish.sh VERSION                   # the site onto GitHub Pages (the ROM-less firmware only)
```

The toolchain and the rest: [BUILDING.md](BUILDING.md) (written for Felucca, the firmware this is built on).

## Risk

Installing custom firmware is at your own risk. If an install fails and the FM-1 no longer starts, recovering it
needs a [Transporter](https://github.com/kurogedelic/FM-1-transporter). The author accepts no responsibility if
your FM-1 is damaged or misbehaves because of this firmware.

## Credits and licence

FMDj, the LSDj fork of [Felucca](https://github.com/hugelton/Felucca): DEADACTIVE. Felucca by Leo Kuroshita
(@kurogedelic), Hügelton Instruments. Game Boy emulation: Peanut-GB (Mahyar Koshkouei, MIT) and minigb_apu (Alex
Baines, Mahyar Koshkouei, MIT). The firmware package also holds three JieLi AC79 SDK files (JieLi Technology,
Apache-2.0): [LICENSING.md](LICENSING.md).

GPL-3.0-only, no warranty: [LICENSE](LICENSE).

M-VAVE and FM-1 are trademarks of their respective owners; Game Boy is a trademark of Nintendo. This project is not
affiliated with any of them.
