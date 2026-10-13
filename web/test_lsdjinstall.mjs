// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
// The LSDj install page's logic (web/lsdjinstall.js) with the WebAssembly (build/web/lsdj_emu.wasm, from
// web/lsdj_wasm.sh; skipped without it), on made-up ROMs: no LSDj data. What needs the Mac build to compare with
// is in tests/lsdj/lsdj_install_test.py.   node web/test_lsdjinstall.mjs
import fs from "node:fs";
import { loadEmu, makeCart, padsFor } from "./lsdjinstall.js";

let fails = 0;
const ok = (c, what) => { console.log(`${what.padEnd(78)} ${c ? "ok" : "FAILED"}`); fails += !c; };

{ // the demo's key list -> the keys of every frame (a 0 bit is a key down)
  const p = padsFor([[2, 4, "t"], [3, 6, "sr"], [9, 99, "a"]], 10);
  ok(p.length === 10 && p[0] === 0xFF && p[2] === 0xF7 && p[3] === 0xE3 && p[4] === 0xEB && p[6] === 0xFF && p[9] === 0xFE,
     "padsFor: the keys of each frame, overlapping presses together, cut at the end");
}

const WASM = new URL("../build/web/lsdj_emu.wasm", import.meta.url);
if (!fs.existsSync(WASM)) {
  console.log("the WebAssembly: skipped (build/web/lsdj_emu.wasm: web/lsdj_wasm.sh)");
} else {
  const emu = await loadEmu(fs.readFileSync(WASM));
  // a ROM of the right kind (MBC5 + RAM + battery, a 128 KiB save, a right header checksum) with this in it
  const rom = (banks, fill) => {
    const r = new Uint8Array(banks * 0x4000);
    for (let i = 0; i < r.length; i++) r[i] = fill(i);
    r[0x147] = 0x1B; r[0x148] = 0; r[0x149] = 0x04;
    let x = 0;
    for (let i = 0x134; i <= 0x14C; i++) x = (x - r[i] - 1) & 0xFF;
    r[0x14D] = x;
    return r;
  };
  const refused = async (r, room) => {
    let ran = 0;
    try { await makeCart(emu, r, null, room, () => { ran++; }); return { said: "", ran }; }
    catch (e) { return { said: e.message, ran }; }
  };
  const noise = (i) => (i * 2654435761 >>> 13) & 0xFF;                   // nothing to trim, nothing to pack
  let a = await refused(rom(64, noise), 479740);
  ok(/needs 10\d{5} bytes .* leaves 479740: \d+ too many/.test(a.said) && a.ran === 0,
     "a ROM far too big is refused at once, before the emulator runs: " + a.said.slice(0, 40));
  a = await refused(rom(2, noise), 100);                                  // (after the big one: its own size counts)
  ok(/needs 3\d{4} bytes .* leaves 100:/.test(a.said) && a.ran === 0, "... and a small one after it by its own size: " + a.said.slice(0, 30));
  a = await refused(rom(2, () => 0xD3), 479740);                         // 0xD3: no instruction of the Game Boy's
  ok(/does not run in the emulator \(LSDj's first start: \d+ errors\)/.test(a.said),
     "a ROM of instructions the Game Boy does not have: refused, with the errors counted: " + a.said.slice(0, 24));
  a = await refused(rom(2, () => 0x00), 479740);                         // NOPs: it runs, and draws nothing
  ok(/the screen stays blank/.test(a.said), "a ROM that runs and draws nothing: refused");
  a = await refused(new Uint8Array(1000), 479740);
  ok(/not an LSDj ROM: it is not 1 to 64 whole banks/.test(a.said), "a file that is not whole banks: refused");
}
console.log(fails ? `test_lsdjinstall: ${fails} FAILED` : "test_lsdjinstall: all passed");
process.exit(fails ? 1 : 0);
