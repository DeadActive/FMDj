// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
// The install page's logic from the command line, for tests/lsdj/lsdj_install_test.py:
//   node web/lsdj_install_cli.mjs WASM ROM DEMO.json BASE.fwsc OUT.cart OUT.fwsc
// DEMO.json: { keys: [[from, to, letters], ...], frames } or null. Prints what the page would say.
import fs from "node:fs";
import { Package } from "./fm1pkg.js";
import { loadEmu, makeCart, baseInfo, withCart } from "./lsdjinstall.js";

const [wasm, rom, demo, base, outCart, outPkg] = process.argv.slice(2);
try {
  const emu = await loadEmu(fs.readFileSync(wasm)), b = new Uint8Array(fs.readFileSync(base));
  const info = baseInfo(new Package(b));
  const cart = await makeCart(emu, new Uint8Array(fs.readFileSync(rom)), JSON.parse(fs.readFileSync(demo, "utf8")), info.room);
  fs.writeFileSync(outCart, cart);
  fs.writeFileSync(outPkg, withCart(b, cart));
  console.log(`block ${cart.length} B of ${info.room} free; firmware ${info.appLen} B`);
} catch (e) {
  console.log("refused: " + e.message);
  process.exit(1);
}
