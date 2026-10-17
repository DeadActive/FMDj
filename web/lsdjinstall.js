// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
// The LSDj install page's logic (web/lsdj_install.html; node-tested by web/test_lsdjinstall.mjs and
// tests/lsdj/lsdj_install_test.py): the user's own ROM file -> the cartridge block -> an installable package, all
// in the browser. The firmware it starts from holds nothing of LSDj (tools/build.py: lsdj-base.fwsc).
//   loadEmu(wasm)       the firmware's emulator and the block's maker as WebAssembly (web/lsdj_wasm.c)
//   makeCart(...)       runs the ROM as the Mac build does (tools/lsdj_rom.py): LSDj's first start from an empty
//                       save, the demo song typed in, a short play; then the block. The same C code, the same bytes.
//                       DMGo! (the owner's own, 2026-10-17): its first start, then a step set and played (that
//                       run's save is not kept); no demo
//   baseInfo(pkg)       how much of the app's flash slot the firmware leaves free
//   withCart(base, c)   the package with the block at the end of the slot, parsed again and checked
import { Package, crc16, productOf } from "./fm1pkg.js";

export const BOOT_FRAMES = 3000;                // 50 s of Game Boy time: past LSDj's first-start check
export const PLAY_FRAMES = 600, PLAY_PEAK = 2000;
export const DMGO_FRAMES = 600;                 // DMGo's first start: its title screen and its empty save
// DMGo's check: past its title (START), the cursor to step 1 (10 x DOWN), B sets it, START plays it
export const DMGO_CHECK = [[300, 304, "t"], ...Array.from({ length: 10 }, (_, i) => [400 + 12 * i, 404 + 12 * i, "d"]),
                           [520, 524, "b"], [560, 564, "t"]], DMGO_CHECK_FRAMES = 800;
const KEY = { a: 1, b: 2, s: 4, t: 8, r: 16, l: 32, u: 64, d: 128 };   // Peanut-GB's joypad bits
const NOT_ROM = { "-3": "it is not 1 to 64 whole banks of 16 KiB",
  "-4": "it is neither LSDj's kind of cartridge (MBC5, a 128 KiB save) nor DMGo's (its title, MBC5, a 32 KiB save)",
  "-5": "its header checksum is wrong" };

export async function loadEmu(wasm) {
  const { instance } = await WebAssembly.instantiate(wasm, {});
  const x = instance.exports;
  if (x._initialize) x._initialize();
  const mem = (at, n) => new Uint8Array(x.memory.buffer, at, n);
  let len = 0;
  return {
    setRom(rom) {
      if (rom.length > 0x100000) return -3;
      mem(x.lw_rom(), rom.length).set(rom);
      len = rom.length;
      return x.lw_check(len);
    },
    start: () => x.lw_start(len),
    prog: () => ["lsdj", "dmgo"][x.lw_prog()] || null,   // the ROM's program (after setRom)
    saveSize: () => x.lw_save_n() >>> 0,
    frames: (n, pad) => x.lw_frames(n, pad),
    errors: () => x.lw_errors() >>> 0,
    peak: () => x.lw_peak() >>> 0,
    ink: () => x.lw_ink() >>> 0,
    sram: () => mem(x.lw_sram(), x.lw_save_n() || 0x20000).slice(),
    setSram: (s) => mem(x.lw_sram(), 0x20000).set(s),
    screen: () => mem(x.lw_fb(), 160 * 144),
    need: () => x.lw_need() >>> 0,
    cart(max) {
      const n = x.lw_cart(max);
      return n < 0 ? n : mem(x.lw_out(), n).slice();
    },
  };
}

// the keys of every frame (Peanut-GB's byte: a 0 bit is a key down) from the demo's list: [from, to, letters]
// as tests/lsdj/lsdj_host.c takes them (-k FROM:TO:KEYS; letters of l r u d a b s(elect) t(start))
export function padsFor(keys, frames) {
  const p = new Uint8Array(frames).fill(0xFF);
  for (const [from, to, letters] of keys) {
    let m = 0;
    for (const c of letters) m |= KEY[c] || 0;
    for (let f = from; f < to && f < frames; f++) p[f] &= ~m;
  }
  return p;
}

async function run(emu, pads, what, say, pause) {
  const r = emu.start();
  if (r) throw new Error(`The emulator does not take this ROM (error ${r}).`);
  for (let f = 0, last = 0; f < pads.length;) {
    let n = 1;
    while (f + n < pads.length && pads[f + n] === pads[f] && n < 600) n++;
    emu.frames(n, pads[f]);
    f += n;
    if (f - last >= 1500 || f === pads.length) {
      last = f;
      say(what, f, pads.length);
      await pause();
    }
  }
  if (emu.errors()) throw new Error(`This ROM does not run in the emulator (${what}: ${emu.errors()} errors).`);
  if (!emu.ink()) throw new Error(`This ROM does not run in the emulator (${what}: the screen stays blank).`);
}

// rom: the file's bytes; demo: { keys, frames } (tools/lsdj_demo.py key_args(60): its list, its end + 60), or null
// for an empty song; room: the slot's free bytes. -> the block. Throws an Error that says why not.
export async function makeCart(emu, rom, demo, room, say = () => {}, pause = async () => {}) {
  const why = emu.setRom(rom);
  if (why) throw new Error(`This file is not an LSDj or DMGo ROM: ${NOT_ROM[why] || "error " + why}.`);
  emu.setSram(new Uint8Array(0x20000));
  const full = (need) => new Error(`This ROM needs ${need} bytes and the FM-1's firmware leaves ${room}: ${need - room} too ` +
    "many. A ROM with fewer kits fits; the kits can go in with the song page afterwards.");
  if (emu.need() > room) throw full(emu.need());                     // (the least it can need: with an empty save)
  if (emu.prog() === "dmgo") return dmgoCart(emu, room, full, say, pause);
  await run(emu, new Uint8Array(BOOT_FRAMES).fill(0xFF), "LSDj's first start", say, pause);
  let save = emu.sram();
  if (demo) {
    emu.setSram(save);
    await run(emu, padsFor(demo.keys, demo.frames), "typing the demo song", say, pause);
    save = emu.sram();
    emu.setSram(save);
    await run(emu, padsFor([[60, 64, "t"]], PLAY_FRAMES), "playing it", say, pause);
    if (emu.peak() < PLAY_PEAK) throw new Error("This ROM runs, but the demo song typed into it makes no sound.");
  }
  emu.setSram(save);
  const cart = emu.cart(room);
  if (cart === -6) throw full(emu.need());
  if (typeof cart === "number") throw new Error(`The cartridge block could not be made (error ${cart}).`);
  return cart;
}

async function dmgoCart(emu, room, full, say, pause) {
  await run(emu, new Uint8Array(DMGO_FRAMES).fill(0xFF), "DMGo's first start", say, pause);
  const save = emu.sram();
  emu.setSram(save);
  await run(emu, padsFor(DMGO_CHECK, DMGO_CHECK_FRAMES), "playing a step", say, pause);
  if (emu.peak() < PLAY_PEAK) throw new Error("This ROM runs, but a step set in DMGo and played makes no sound.");
  emu.setSram(save);
  const cart = emu.cart(room);
  if (cart === -6) throw full(emu.need());
  if (typeof cart === "number") throw new Error(`The cartridge block could not be made (error ${cart}).`);
  return cart;
}

const slotOf = (p) => p.dec.subarray(p.appOff, p.appOff + p.app.size);

// pkg: a parsed lsdj-base package. -> { size, appLen, room }: the slot, the firmware's own bytes, what is free.
// tools/build.py ends the firmware with "LSDE" and its length; anything else is not an LSDj base (or has a ROM in)
export function baseInfo(pkg) {
  const s = slotOf(pkg);
  let e = s.length;
  while (e && s[e - 1] === 0xFF) e--;
  const mark = e >= 8 && String.fromCharCode(...s.subarray(e - 8, e - 4)) === "LSDE";
  const len = mark ? (s[e - 4] | s[e - 3] << 8 | s[e - 2] << 16 | s[e - 1] << 24) >>> 0 : 0;
  if (!mark || len !== e) throw new Error("This is not the LSDj firmware without a ROM (no end mark in it).");
  return { size: s.length, appLen: e, room: s.length - e };
}

// base: the lsdj-base package's bytes; cart: the block. -> the package to install. Everything is checked on the
// result parsed again: the FM-1's loader writes the app without checking the app's own checksum.
export function withCart(base, cart) {
  const pkg = new Package(base), info = baseInfo(pkg);
  if (cart.length > info.room) throw new Error(`the block is ${cart.length} bytes and ${info.room} are free`);
  if (String.fromCharCode(...cart.subarray(cart.length - 16, cart.length - 12)) !== "LSDC") throw new Error("not a cartridge block");
  const slot = slotOf(pkg).slice();
  slot.set(cart, slot.length - cart.length);
  pkg.setApp(slot);
  const out = pkg.serialize(), back = new Package(out), got = slotOf(back), h = back.head;
  const same = got.length === slot.length && got.every((v, i) => v === slot[i]);
  if (!same || crc16(got) !== back.app.dcrc || crc16(back.dec.subarray(h.hdrOff + 32, h.hdrOff + h.size)) !== h.dcrc ||
      productOf(out) !== productOf(base) || out.length !== base.length)
    throw new Error("The package made here does not check out; nothing was sent to the FM-1.");
  return out;
}
