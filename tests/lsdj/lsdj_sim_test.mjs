// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
// The LSDj simulator's core (web/sim/lsdjsim.c as wasm) through the page's engine (web/sim/engine.js), with a small
// Game Boy program of our own as the cartridge (no LSDj data): PLAY starts it, it runs at the Game Boy's speed with
// sound and a picture, REC held saves to the flash the page keeps, and without a cartridge PLAY starts nothing.
// The firmware's console is there too (`ble ...`), so the song page's own code moves saves in and out of it.
// Needs build/web/sim/lsdjsim.wasm (web/sim/build.sh) and build/web/lsdj_emu.wasm (web/lsdj_wasm.sh); skipped
// without them.   node tests/lsdj/lsdj_sim_test.mjs
import fs from "node:fs";
import { Fm1Sim, FS, SECTOR } from "../../web/sim/engine.js";
import { BTN, B, ENC, KEYBOARD, GB, STEP, setProgram } from "../../web/sim/controls.js";
import { LAYOUTS } from "../../web/sim/layout.js";
import { loadEmu, makeCart, cartProg } from "../../web/lsdjinstall.js";
import { simLink } from "../../web/sim/simlink.js";
import { Link, listSongs, downloadSav, uploadSav, saveToFlash } from "../../web/lsdjlink.js";

let fails = 0;
const ok = (c, what) => { console.log(`${what.padEnd(78)} ${c ? "ok" : "FAILED"}`); fails += !c; };
const root = new URL("../../", import.meta.url);

{ // the page's control ids are the firmware's (panel.c), and every control has a place in both layouts
  const panel = fs.readFileSync(new URL("firmware/src/panel.c", root), "utf8");
  const en = (first) => panel.slice(panel.indexOf("enum { " + first)).split("}")[0].replace(/enum \{/, "").split(",").map((s) => s.trim());
  ok(BTN.every((b, i) => en("B_FX")[i] === b.c), "the buttons' ids are panel.c's, in its order");
  ok(ENC.every((e, i) => en("EN_SELECT")[i] === e.c), "the knobs' ids are panel.c's, in its order");
  ok(Object.values(LAYOUTS).every((L) => BTN.every((_, i) => L.ctl[`btn:${i}`]) && [...Array(27).keys()].every((n) => L.ctl[`key:${n}`])),
     "every button and key has a place in both layouts");
  ok(KEYBOARD.Enter.id === B.PLAY && KEYBOARD.KeyX.id === 24 && KEYBOARD.KeyZ.id === 26 && KEYBOARD.ShiftLeft.id === B.SEQ &&
     GB[`btn:${B.PLAY}`] === "START" && GB["key:24"] === "A", "the keyboard: Enter START, X A, Z B, Shift SELECT");
}

const WASM = new URL("build/web/sim/lsdjsim.wasm", root), EMU = new URL("build/web/lsdj_emu.wasm", root);
if (!fs.existsSync(WASM) || !fs.existsSync(EMU)) {
  console.log("the simulator: skipped (build/web/sim/lsdjsim.wasm: web/sim/build.sh; build/web/lsdj_emu.wasm: web/lsdj_wasm.sh)");
} else {
  // our own program (as tests/lsdj/lsdj_install_test.py test_rom): a timer interrupt, a note every 64th, one dark row a tile
  const rom = new Uint8Array(0x8000).fill(0xFF);
  const at = (a, ...b) => rom.set(b, a);
  at(0x40, 0xF5, 0x3E, 0x01, 0xEA, 0x01, 0xC1, 0xF1, 0xD9);
  at(0x50, 0xF5, 0x3E, 0x01, 0xEA, 0x00, 0xC1, 0xF1, 0xD9);
  at(0x100, 0x00, 0xC3, 0x50, 0x01);
  rom.fill(0, 0x104, 0x150);
  rom[0x147] = 0x1B; rom[0x148] = 0; rom[0x149] = 0x04;
  let x = 0;
  for (let i = 0x134; i <= 0x14C; i++) x = (x - rom[i] - 1) & 0xFF;
  rom[0x14D] = x;
  at(0x150, 0xF3, 0x31, 0xFF, 0xDF, 0x3E, 0x80, 0xE0, 0x26, 0x3E, 0x77, 0xE0, 0x24, 0x3E, 0xFF, 0xE0, 0x25,
     0x3E, 0xB0, 0xE0, 0x11, 0x3E, 0xF0, 0xE0, 0x12, 0x3E, 0x83, 0xE0, 0x13, 0x3E, 0x49, 0xE0, 0x06, 0xE0, 0x05, 0x3E, 0x06, 0xE0, 0x07,
     0xAF, 0xE0, 0x0F, 0xEA, 0x00, 0xC1, 0xEA, 0x01, 0xC1, 0xEA, 0x02, 0xC1, 0x21, 0x00, 0x80, 0x3E, 0xFF, 0x22, 0x77,
     0x3E, 0x05, 0xE0, 0xFF, 0xFB, 0xC3, 0x00, 0x02);
  at(0x200, 0xAF, 0x21, 0x00, 0xC1, 0xB6, 0xC2, 0x40, 0x02, 0xAF, 0x21, 0x01, 0xC1, 0xB6, 0xCA, 0x00, 0x02);
  at(0x210, 0xAF, 0xEA, 0x01, 0xC1, 0xC3, 0x00, 0x02);
  at(0x240, 0xAF, 0xEA, 0x00, 0xC1, 0x21, 0x02, 0xC1, 0x34, 0x7E, 0xE6, 0x3F, 0xC2, 0x00, 0x02, 0x3E, 0xC6, 0xE0, 0x14, 0xC3, 0x00, 0x02);
  const cart = await makeCart(await loadEmu(fs.readFileSync(EMU)), rom, null, 1 << 20);
  const wasm = fs.readFileSync(WASM);
  const run = (sim, seconds) => {                  // as the worklet: 128 frames at a time -> the loudest sample
    let peak = 0;
    for (let n = 0; n < seconds * FS; n += 128) {
      const { left, right } = sim.render(128);
      for (let i = 0; i < 128; i++) peak = Math.max(peak, Math.abs(left[i]), Math.abs(right[i]));
    }
    return peak;
  };
  const tap = (sim, label) => { sim.btn(label, true); run(sim, 0.1); sim.btn(label, false); };
  const colours = (fb, y0, y1) => { const s = new Set(); for (let i = y0 * 240; i < y1 * 240; i++) s.add(fb[i]); return s.size; };

  { // no cartridge: PLAY starts nothing
    const sim = await Fm1Sim.create(wasm);
    ok(sim.boot(null, null) === true, "a first visit: no flash kept");
    run(sim, 0.3);
    ok(colours(sim.fb(), 90, 230) > 4, "the start screen is drawn (FMDJ, the keys' lines)");
    tap(sim, B.PLAY);
    run(sim, 0.5);
    ok(!sim.playing() && sim.error() === -3 && sim.frames() === 0, "without a cartridge PLAY starts nothing (NO LSDJ ROM)");
  }
  let sectors;
  { // our program as the cartridge
    const sim = await Fm1Sim.create(wasm);
    sim.boot(null, cart);
    run(sim, 0.3);
    ok(!sim.playing() && sim.frames() === 0, "nothing runs before PLAY");
    tap(sim, B.PLAY);
    const f0 = sim.frames(), peak = run(sim, 3);
    ok(sim.playing() && sim.error() === 0, "PLAY starts the Game Boy");
    ok(sim.frames() - f0 >= 176 && sim.frames() - f0 <= 183, `it runs at the Game Boy's speed (${sim.frames() - f0} frames in 3 s of sound)`);
    ok(peak > 0.02 && peak <= 1, `with sound (peak ${peak.toFixed(3)})`);
    const fb = sim.fb();
    ok(colours(fb, 4, 220) >= 2 && colours(fb, 222, 240) > 2, "its picture on the screen, the status line under it");
    sim.enc(0, 3); sim.enc(1, -2); sim.enc(2, 1); sim.key(24, true); sim.master(300);
    run(sim, 0.5);
    sim.releaseAll();
    ok(sim.error() === 0 && sim.takeDirty().length === 0, "knobs and keys are taken; nothing is written to flash by itself");
    sim.btn(B.REC, true);                          // REC held for a second: the save goes to flash
    run(sim, 1.5);
    sim.btn(B.REC, false);
    run(sim, 0.2);
    sectors = sim.takeDirty();
    ok(sectors.length > 0 && sectors.every((s) => s.off >= 0xA0000 && s.off < 0xDC000 && s.bytes.length === SECTOR),
       `REC held saves: ${sectors.length} sectors of the store's area to keep`);
  }
  { // a key down and up again between two passes of the main loop (a quick tap, a script): still a press
    const sim = await Fm1Sim.create(wasm);
    sim.boot(null, cart);
    run(sim, 0.2);
    sim.btn(B.PLAY, true);
    sim.btn(B.PLAY, false);
    run(sim, 0.5);
    ok(sim.playing(), "a press shorter than a pass of the main loop still counts");
  }
  { // the console: the song page's code (web/lsdjlink.js) against the simulator, as against an FM-1 on USB
    const sim = await Fm1Sim.create(wasm);
    sim.boot(null, cart);
    run(sim, 0.2);
    ok(sim.console("ble").includes("lsdj stopped"), "the console answers `ble` with the unit's status");
    ok(sim.console("ble hold") === "err not started\r\n", "... and a transfer command with its reply");
    tap(sim, B.PLAY);
    run(sim, 0.5);
    const link = simLink(Link, async (line) => { const t = sim.console(line); run(sim, 0.02); return t; });   // (time goes on)
    const inf = await link.info();
    ok(inf.started && inf.version === 3 && inf.flash && inf.banks === 2 && inf.prog === "lsdj" && inf.size === 0x20000, "info: started, with flash, our program's 2 banks");
    const sav = new Uint8Array(0x20000);             // an LSDj save with an empty file system, and a mark of ours
    sav[0x813E] = 0x6A; sav[0x813F] = 0x6B; sav[0x8140] = 0xFF;
    sav.fill(0xFF, 0x8141, 0x8200);
    sav.set([0xDE, 0xAD, 0xBE, 0xEF], 0x1F000);
    const f0 = sim.frames(), up = await uploadSav(link, sav);
    ok(up.unsaved === null && sim.takeDirty().length > 0, "a whole save goes in, and into the flash the page keeps");
    run(sim, 0.5);
    ok(sim.frames() > f0 && sim.playing(), "... the Game Boy restarted with it and runs");
    const got = await downloadSav(link);
    ok(got.length === 0x20000 && got[0x1F000] === 0xDE && got[0x1F003] === 0xEF && got[0x813E] === 0x6A, "the save comes out again, with what went in");
    const l = await listSongs(link);
    ok(l.list.length === 0 && l.free === 191, "its song list is read (empty: 191 free blocks)");
    await saveToFlash(link);
    ok(true, "Save now is carried out");
  }
  { // the next visit: the kept sectors are the flash again
    const sim = await Fm1Sim.create(wasm);
    ok(sim.boot(new Map(sectors.map((s) => [s.off, s.bytes])), cart) === false, "the next visit starts from the kept flash");
    tap(sim, B.PLAY);
    run(sim, 1);
    ok(sim.playing() && sim.error() === 0 && sim.frames() > 50, "... and PLAY loads the save and runs");
  }
  ok(cartProg(cart) === "lsdj" && cartProg(null) === null && cartProg(new Uint8Array(40)) === null,
     "the program of a kept cartridge: LSDj's kind; none without one");
  const DMGO = process.env.DMGO_ROM;               // the owner's own DMGo (v1), through the firmware's lsdj.c
  if (!DMGO) console.log("DMGo in the simulator: skipped (DMGO_ROM)");
  else {
    const dcart = await makeCart(await loadEmu(fs.readFileSync(EMU)), new Uint8Array(fs.readFileSync(DMGO)), null, 1 << 20);
    ok(cartProg(dcart) === "dmgo", "the program of a kept cartridge: DMGo");
    const sim = await Fm1Sim.create(wasm);
    sim.boot(null, dcart);
    tap(sim, B.PLAY);                              // the firmware starts the Game Boy
    run(sim, 1);
    tap(sim, B.PLAY);                              // START: past DMGo's title, on CH1
    run(sim, 1);
    ok(sim.playing() && sim.error() === 0, "DMGo runs in the simulator");
    const rows = (fb, y0, y1, x1 = 240) => Array.from({ length: y1 - y0 }, (_, k) => [...fb.slice((y0 + k) * 240, (y0 + k) * 240 + x1)]).flat();
    const same = (a, b) => a.every((v, i) => v === b[i]);
    // on the screen (1.5x from row 4): DMGo's SEQ row, Game Boy rows 88..95; the top row's left part ("CH1:PULSE";
    // its right part, the BPM under the cursor, blinks)
    const SEQ = [4 + 88 * 3 / 2, 4 + 96 * 3 / 2], TOP = [4, 4 + 12, 110];
    const seq0 = rows(sim.fb(), ...SEQ), top0 = rows(sim.fb(), ...TOP);
    sim.key(0, true); run(sim, 0.1); sim.key(0, false); run(sim, 0.5);   // F3: step 1
    ok(!same(seq0, rows(sim.fb(), ...SEQ)) && same(top0, rows(sim.fb(), ...TOP)),
       "a white key sets its step: the step row changes, the top does not");
    sim.key(24, true); run(sim, 0.1); sim.key(24, false); run(sim, 0.5);   // F5: LSDj's A, DMGo's step 15
    ok(same(top0, rows(sim.fb(), ...TOP)), "the top F is a step in DMGo, not A (no menu)");
    sim.key(22, true); run(sim, 0.1); sim.key(22, false); run(sim, 0.5);   // D#5: DMGo's A opens its menu
    ok(!same(top0, rows(sim.fb(), ...TOP)), "D#5 is A: DMGo's top menu opens");
    ok(sim.error() === 0 && sim.playing(), "... and it still runs");
  }
  setProgram("dmgo");                              // the page with DMGo kept: its keys on the panel and keyboard
  ok(GB["key:22"] === "A" && GB["key:25"] === "B" && !GB["key:24"] && !GB["key:26"] && STEP["key:0"] === "1" &&
     STEP["key:26"] === "16" && Object.keys(STEP).length === 16 && KEYBOARD.KeyX.id === 22 && KEYBOARD.KeyZ.id === 25 &&
     !GB[`btn:${B["OCT+"]}`] && !GB["enc:2"], "DMGo's panel: A D#5, B F#5, the white keys steps 1..16, no screen shortcuts");
}
console.log(fails ? `lsdj_sim_test: ${fails} FAILED` : "lsdj_sim_test: all passed");
process.exit(fails ? 1 : 0);
