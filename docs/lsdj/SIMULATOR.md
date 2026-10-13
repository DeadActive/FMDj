# LSDj on the FM-1: the simulator page

`webapp/lsdj-sim/` on the site (`web/sim/` here): the FM-1's LSDj firmware running in a browser, behind the FM-1's
panel drawn in the original Game Boy's colours. It is built after Hortator's simulator and uses the same parts.

## How it is made

- **The firmware, not a copy of it.** `web/sim/lsdjsim.c` compiles the LSDj unit (`firmware/src/lsdj/lsdj.c`) and the
  core's own text drawing (`gfx.c`, the fonts) to WebAssembly as they are. What stands in for the hardware is in that
  file: the screen (a 240 x 240 frame buffer), the buttons, keys, knobs and MASTER pot, the sound (pulled in blocks of
  128 frames, as the audio interrupt does), time (counted in sound: a pass of the main loop every 15 ms of it), the
  flash (in memory, with a map of written sectors).
- **The same interface as Hortator's simulator** (`sim_init`, `sim_render`, `sim_btn`, `sim_key`, `sim_enc`, ...), so
  its engine, worklet, controls, layout and flash store carry over (`web/sim/engine.js`, `worklet.js`, `controls.js`,
  `layout.js`, `store.js`, `audio.js`). The firmware runs in the audio thread; the audio is its clock.
- **Your own ROM.** Nothing of LSDj is in the page or the WebAssembly. The page asks for the ROM file once, makes the
  cartridge block from it with the install page's code (`web/lsdjinstall.js`: LSDj's first start, the demo song typed
  in), and keeps the block in the browser (IndexedDB `fm1-lsdj-sim`). REC held saves into the same browser storage.
- **The look** is CSS only (`web/sim/sim.css`); positions are the FM-1's own (`layout.js`). The screen's greys are
  shown as the Game Boy screen's greens (`app.js`).

## What differs from the FM-1

- `LOAD` on the status line is 0 %: the page does not model the FM-1's processor, so it says nothing about speed on
  the device.
- No USB. The firmware's console is there all the same (`sim_con`), and the page uses it: see below.
- The four knobs on the right, GLO and the keys other than the top F and G do nothing in the LSDj build, as on the
  device; they are drawn dimmed.

## Songs and saves

Under the device, once the power is on: load a project or song file (.lsdprj, .lsdsng), restore or download the whole
save (.sav), download or delete a song of LSDj's list, Save now. This is the song page's own code (`web/lsdjlink.js`,
`web/lsdjfs.js`): `web/sim/simlink.js` hands its console lines (`ble hold`, `ble w ...`) to the simulated firmware,
which answers as an FM-1 does over USB. A project's kits go into the simulated flash's kit banks. As on the FM-1, a
loaded project is opened in LSDj itself (PROJECT, LOAD/SAVE SONG, LOAD), and everything is kept by saving to flash,
which here is the browser's storage.

## Build and test

    ./build.sh                 once: the generated fonts (build/gen)
    web/lsdj_wasm.sh           the emulator and the block's maker (also the install page's)
    web/sim/build.sh           the simulator: build/web/sim/lsdjsim.wasm
    python3 web/make_site.py build/lsdj-private.fwsc VERSION build/sites/NAME
    node tests/lsdj/lsdj_sim_test.mjs

The test runs a small Game Boy program of our own as the cartridge: PLAY starts it, it runs at the Game Boy's speed
with sound and a picture, REC held saves, and a press shorter than a pass of the main loop still counts.
