# FMDj: the landing page and the new name (design, 2026-10-17)

The owner: "create a landing page for lsdj, the name of project now is FMDj"; screenshots on top; the screenshots
from their project `~/Downloads/dontstop/DONTSTOP_v682-v901.sav`. Approved with: the name everywhere, screenshots on
top (not the live simulator).

## The name

The project is **FMDj** (LSDj on the M-VAVE FM-1). Everywhere a person reads the project's name: the pages' titles and
headings (installer with your ROM, song page, simulator), the simulator's printed name (`FMDJ · FM-1`), the docs, and
the FM-1's own start screen (`FMDJ` instead of `LSDJ`) if the firmware's update-path gate stays green; otherwise that
one stays `LSDJ`. Where the text means LSDj itself (the program, the ROM: `NO LSDJ ROM`, "your own LSDj ROM"), it
stays LSDj. File, branch and package names and the device identity do not change.

## The landing page

The site's root page (today a redirect to the installer). Top to bottom:

1. **Screenshots on top**: the FM-1 drawn as on the simulator (its Game Boy colours), with real LSDj screens cycling in
   its screen, and a row of the same screens under it. The name FMDj and one line: LSDj, the Game Boy tracker, inside
   the M-VAVE FM-1.
2. **What it is**: LSDj runs in a Game Boy emulator inside the FM-1, with the owner's own ROM; nothing of LSDj ships.
3. **What it does**: songs and kits over USB (the song page), saving to the FM-1's flash, the sound timing and speed
   work, the installer that puts your ROM in, the simulator in the browser.
4. **Controls**: the FM-1 to Game Boy table and the computer keyboard's keys (as the simulator page has them).
5. **Put it on your FM-1**: steps, links to the installer with your ROM, the song page, the simulator.
6. **Credits and licence**: LSDj by Johan Kotlinski (not included, bring your own), Peanut-GB, minigb_apu, Felucca,
   Hortator's simulator; the fork credited as DEADACTIVE; trademarks.

Style: the simulator's Game Boy look for the whole page. Local assets only (no CDN).

## The screenshots

A tool runs the owner's ROM with a save on the Mac's emulator (`tests/lsdj/lsdj_host.c`), starts the song, walks to
each screen (SONG, CHAIN, PHRASE, INSTR, TABLE, SYNTH, WAVE, GROOVE, PROJECT) and writes each picture as a PNG in the
Game Boy's greens. They are made from the owner's ROM: like the private build they live in git-ignored `build/` and
go only onto the local site; publishing them is the owner's decision. Without them the page shows the device with an
empty screen and no gallery.
