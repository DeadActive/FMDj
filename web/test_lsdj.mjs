// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// Node checks of the LSDj song page's JS (no browser, no hardware, no LSDj data). Run from the repo root:
//   node web/test_lsdj.mjs
// - lsdjfs.js: the save's file system and song files, on songs made here
// - lsdjlink.js: the console protocol and the page's flows against a simulated FM-1, which must answer
//   build/tests/lsdj_xfer_vectors.txt (written by tests/lsdj/lsdj_xfer_test.c) as the firmware does
// - lsdj.html with both modules inlined (as web/make_site.py does) is one script that compiles

import * as fs from "./lsdjfs.js";
import { existsSync, readFileSync } from "node:fs";
import vm from "node:vm";
import * as ln from "./lsdjlink.js";

let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(72)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const throws = (fn, re) => { try { fn(); } catch (e) { return re.test(e.message); } return false; };

/* ---- a save and songs made here ---- */
function emptySav() {
    const sav = new Uint8Array(fs.SAV_SIZE);
    sav.fill(0xFF, fs.ACTIVE, fs.BLOCKS);                // no active slot, every block free
    sav[0x813E] = 0x6A; sav[0x813F] = 0x6B;              // "jk": LSDj's mark of an initialised file system
    return sav;
}
function testSong(seed) {                                // 32 KiB with escapes, runs and two kit instruments
    const s = new Uint8Array(0x8000);
    for (let i = 0; i < 0x1000; i++) s[i] = (i * 13 + seed) & 0xFF;      // every byte value, 0xC0 and 0xE0 too
    s.fill(0xC0, 0x1000, 0x1040);                        // a run of an escape byte
    s.fill(0xE0, 0x1040, 0x1043);                        // three: too short for a run here
    for (let i = 0; i < 0x40; i++) s.set([0xA8, 0, 0, 0xFF, 0, 0, 3, 0, 0, 0xD0, 0, 0, 0, 0xF3, 0, 0], 0x3080 + 16 * i);
    s.set([2, 0x11, 0x83, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x05, 0x88], 0x3080);          // kits 3 (bit 7 set), 5
    s.set([2, 0x12, 0x03, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x03, 0x89], 0x3080 + 16 * 7); // kits 3, 3
    return s;
}
function packSong(song) {                                // tokens: literals with escapes, runs of 4 or more
    const t = [];                                        // (no run of 0xC0: C0 C0 n would read as an escaped C0)
    for (let i = 0; i < song.length;) {
        let n = 1;
        while (i + n < song.length && song[i + n] === song[i] && n < 255) n++;
        if (n >= 4 && song[i] !== 0xC0) { t.push([0xC0, song[i], n]); i += n; }
        else { t.push(song[i] === 0xC0 ? [0xC0, 0xC0] : song[i] === 0xE0 ? [0xE0, 0xE0] : [song[i]]); i++; }
    }
    return t;
}
function addPacked(sav, slot, song, name, version) {     // the song into free blocks, as LSDj lays it out
    const blocks = [[]];
    for (const tok of packSong(song)) {
        if (blocks.at(-1).length + tok.length > fs.BLOCK - 2) blocks.push([]);
        blocks.at(-1).push(...tok);
    }
    const ids = [];
    for (let i = 0; i < fs.NBLOCKS && ids.length < blocks.length; i++) if (sav[fs.FAT + i] >= fs.SLOTS) ids.push(i);
    blocks.forEach((b, k) => {
        b.push(0xE0, k + 1 < blocks.length ? ids[k + 1] + 1 : 0xFF);
        sav[fs.FAT + ids[k]] = slot;
        sav.fill(0, fs.BLOCKS + fs.BLOCK * ids[k], fs.BLOCKS + fs.BLOCK * (ids[k] + 1));
        sav.set(b, fs.BLOCKS + fs.BLOCK * ids[k]);
    });
    sav.fill(0, fs.NAMES + 8 * slot, fs.NAMES + 8 * slot + 8);
    sav.set([...name].map(c => c.charCodeAt(0)), fs.NAMES + 8 * slot);
    sav[fs.VERSIONS + slot] = version;
    return ids;
}
const kitBankData = n => { const b = new Uint8Array(fs.BANK); b[0] = 0x60; b[1] = 0x40; b.fill(n, 2); return b; };

/* ---- lsdjfs ---- */
{
    ok(fs.kitBank(0) === 8 && fs.kitBank(18) === 26 && fs.kitBank(19) === 32 && fs.kitBank(50) === 63, "kit number -> ROM bank");
    ok([8, 26, 32, 33, 63].every(b => fs.kitBank(fs.bankKit(b)) === b) && fs.bankKit(32) === 19, "ROM bank -> kit number");
    ok(fs.crc32(new TextEncoder().encode("123456789")) === 0xCBF43926, "CRC-32 check value");

    const sav = emptySav(), songA = testSong(1), songB = testSong(9);
    ok(fs.songs(sav).length === 0 && fs.freeBlocks(sav) === 0xBF, "an empty save: no songs, 191 free blocks");
    const idsA = addPacked(sav, 0, songA, "ABC 9", 0x12), idsB = addPacked(sav, 5, songB, "ZZTOP123", 4);
    ok(idsA.length > 1 && idsB[0] === idsA.length, "the test songs span several blocks");
    const list = fs.songs(sav);
    ok(list.length === 2 && list[0].slot === 0 && list[0].name === "ABC 9" && list[0].version === 0x12 &&
       list[0].blocks === idsA.length && list[1].slot === 5 && list[1].name === "ZZTOP123", "the song list");
    sav[fs.NAMES + 2] = 0x21;                            // a character outside A-Z 0-9 reads as a space
    ok(fs.nameText(sav, 0) === "AB  9", "names: only A-Z and 0-9");
    sav[fs.NAMES + 2] = 0x43;
    ok(fs.freeBlocks(sav) === 0xBF - idsA.length - idsB.length, "free blocks");

    const d = fs.decodeSong(sav, 0);
    ok(d && eq(d.song, songA) && eq(fs.unpackSong(sav, 5), songB), "songs unpack to their 32 KiB");
    ok(d.src[0x3082] >= fs.BLOCKS && sav[d.src[0x3082]] === 0x83 && d.src[0x1000] === -1, "each literal byte knows its place in the save");
    ok(fs.unpackSong(sav, 3) === null, "an empty slot unpacks to nothing");
    ok(eq(fs.usedKits(songA), [3, 5]) && eq(fs.songKitBanks(sav, 0), [11, 13]), "the kits a song uses, as ROM banks");

    const file = fs.buildSongFile(sav, 0, [kitBankData(1), kitBankData(2)]);
    ok(file.length === 9 + fs.BLOCK * idsA.length + 2 * fs.BANK && file[8] === 0x12 && file[0] === 0x41, "a song file: name, version, blocks, kits");
    ok(fs.songFileName(sav, 0) === "ABC_9-v12.lsdprj", "the file name carries the song's version");
    const parsed = fs.parseSongFile(file);
    ok(parsed.blocks.length === idsA.length && parsed.kits.length === 2 && parsed.kits[1][2] === 2 && parsed.version === 0x12, "a song file parses back");
    ok(fs.parseSongFile(file.subarray(0, 9 + fs.BLOCK * idsA.length)).kits.length === 0, "an .lsdsng (no kits) parses");

    /* into another save whose first slot and first blocks are taken */
    const other = emptySav();
    addPacked(other, 0, songB, "TAKEN", 1);
    const before = other.slice();
    const r = fs.importSong(other, parsed, [40, 9]);     // the kits are in banks 40 and 9 over there
    ok(eq(other, before), "an import changes a copy, not the save it was given");
    ok(r.slot === 1 && fs.songs(r.sav).length === 2 && fs.songs(r.sav)[1].name === "ABC 9", "the song takes the first free slot");
    const got = fs.unpackSong(r.sav, 1), want = songA.slice();
    want[0x3082] = 0x80 | fs.bankKit(40); want[0x3089] = fs.bankKit(9);
    want[0x3080 + 16 * 7 + 2] = fs.bankKit(40); want[0x3080 + 16 * 7 + 9] = fs.bankKit(40);
    ok(got && eq(got, want), "the song arrives whole, its kits renumbered, the other bits kept");
    ok(eq(fs.unpackSong(r.sav, 0), songB), "the song that was there is untouched");
    const mine = fs.slotBlocks(r.sav, 1);
    ok(mine[0] === fs.slotBlocks(other, 0).length && mine.every((b, k) => k + 1 === mine.length ||
       r.sav[fs.BLOCKS + fs.BLOCK * b + fs.nextBlockPtr(r.sav.subarray(fs.BLOCKS + fs.BLOCK * b, fs.BLOCKS + fs.BLOCK * (b + 1)))] === mine[k + 1] + 1),
       "each block's jump names the next block, counted from 1");
    ok(eq(fs.songKitBanks(r.sav, 1), [9, 40]), "over there the song names banks 9 and 40");
    const again = fs.importSong(r.sav, fs.parseSongFile(fs.buildSongFile(r.sav, 1, [kitBankData(2), kitBankData(1)])), [9, 40]);
    ok(eq(fs.unpackSong(again.sav, 2), want), "exported and imported again: the same song");
    ok(eq(fs.unpackSong(fs.importSong(other, fs.parseSongFile(file.subarray(0, 9 + fs.BLOCK * idsA.length))).sav, 1), songA), "an .lsdsng keeps its kit numbers");

    /* refusals */
    ok(throws(() => fs.parseSongFile(file.subarray(0, 300)), /too short/), "a file shorter than a block is refused");
    ok(throws(() => fs.parseSongFile(file.subarray(0, 9 + fs.BLOCK)), /ends inside/), "a file cut inside its blocks is refused");
    ok(throws(() => fs.parseSongFile(file.subarray(0, file.length - 1)), /whole 16 KiB/), "kit data that is not whole banks is refused");
    const noEnd = file.slice(0, 9 + fs.BLOCK); noEnd.fill(0x11, 9);
    ok(throws(() => fs.parseSongFile(noEnd), /corrupted/), "a block without a jump or an end is refused");
    const short = fs.parseSongFile(file); short.blocks.pop();
    ok(throws(() => fs.importSong(other, short, [40, 9]), /no end/), "a song whose last block has no end mark is refused");
    const holed = fs.parseSongFile(file); holed.blocks.splice(1, 1);
    ok(throws(() => fs.importSong(other, holed, [40, 9]), /does not unpack/), "a song that does not unpack whole is refused");
    ok(throws(() => fs.importSong(other, parsed, [40]), /kits/), "a kit list of the wrong length is refused");
    const full = emptySav(); for (let s = 0; s < 32; s++) full[fs.FAT + s] = s;
    ok(throws(() => fs.importSong(full, parsed, [40, 9]), /no free song slot/), "32 songs: refused");
    const tight = emptySav(); tight.fill(0, fs.FAT + 1, fs.FAT + fs.NBLOCKS);
    ok(throws(() => fs.importSong(tight, parsed, [40, 9]), /not enough room/), "too few free blocks: refused");

    /* Review Focus 5: a renumbered kit byte never becomes an escape (0xC0 / 0xE0) */
    const esc = testSong(1); esc[0x3082] = 0xC3;         // kit 3 with both top bits set
    const sav2 = emptySav(); addPacked(sav2, 0, esc, "ESC", 1);
    const f2 = fs.parseSongFile(fs.buildSongFile(sav2, 0, [kitBankData(1), kitBankData(2)]));
    ok(throws(() => fs.importSong(emptySav(), f2, [8, 9]), /cannot be renumbered/), "a renumbered kit byte never becomes an escape (kit 0 -> 0xC0)");
    ok(throws(() => fs.importSong(emptySav(), f2, [45, 9]), /cannot be renumbered/), "... nor 0xE0 (kit 32)");
    ok(fs.unpackSong(fs.importSong(emptySav(), f2, [11, 13]).sav, 0)[0x3082] === 0xC3, "the same kit numbers need no change");

    const gone = fs.removeSong(r.sav, 0);
    ok(fs.songs(gone).length === 1 && fs.songs(gone)[0].slot === 1 && fs.freeBlocks(gone) === 0xBF - mine.length && eq(fs.unpackSong(gone, 1), want), "a removed song frees its slot and blocks");
    const act = r.sav.slice(); act[fs.ACTIVE] = 0;
    ok(fs.removeSong(act, 0)[fs.ACTIVE] === 0xFF && fs.removeSong(act, 1)[fs.ACTIVE] === 0, "removing the active song clears the mark");
}

/* ---- a simulated FM-1: the console, whose `ble WORDS` are the commands of firmware/src/lsdj/lsdj_xfer.c ---- */
class Sim {
    constructor(sram, banks) {                          // banks: the stored bytes of each (the rest reads FF)
        Object.assign(this, { sram, banks, started: false, held: false, resets: 0, line: "", log: [], link: null,
                              mute: false, corrupt: 0, failWrite: -1, failFrom: Infinity, writes: 0, hasLsdj: true, onHold: null, latency: 0,
                              // the flash store: K kit banks of nkit, shown as ROM banks kitBank[]; a queued request
                              kits: 0, nkit: 0, kitBank: [], used: 0, saved: false, kit: new Uint8Array(0x4000), kitOpen: 0,
                              req: 0, reqArg: 0, res: 0, manual: false, slow: 0, v1: false, prog: 0, size: sram.length, flashImage: null, saves: 0, fullOver: Infinity, failReq: 0, noBank: false });
    }
    cap() { return (29 - 2 * this.kits) * 4096 - 256; }    // (lsdj_store.c: two slots of 29 - 2K sectors, a 256-byte head)
    runReq() {                                          // as lsdj.c's step: the queued request, carried out
        const [req, j] = [this.req, this.reqArg];
        this.req = 0; this.res = 0;
        if (this.failReq) { this.res = this.failReq; if (req === 3) this.banks[this.kitBank[j]] = new Uint8Array(0); return; }   // the flash failed (an erase half done)
        if (req === 1) {                                // save: the "packed size" here is the count of bytes not 0
            const n = this.sram.reduce((a, v) => a + (v ? 1 : 0), 0);
            if (n > Math.min(this.cap(), this.fullOver)) this.res = 1;
            else { this.used = n; this.saved = true; this.flashImage = this.sram.slice(); this.saves++; }
        } else if (req === 2) this.banks[this.kitBank[j]] = this.kit.slice();
        else if (req === 3) this.banks[this.kitBank[j]] = new Uint8Array(0);
        else if (req === 4) {
            const was = this.kits; this.kits = j;
            if (this.noBank || this.used > this.cap()) { this.kits = was; this.res = 1; }
            else for (let k = was; k < j; k++) this.banks[this.kitBank[k]] = new Uint8Array(0);
        }
    }
    bankByte(b, i) { return i < this.banks[b].length ? this.banks[b][i] : 0xFF; }
    data(d) { return `ok ${ln.hexBytes(d)} ${ln.crc16(d).toString(16).toUpperCase().padStart(4, "0")}`; }
    exec(text) {
        const w = text.trim().split(/ +/), num = s => (/^[0-9A-Fa-f]{1,8}$/.test(s || "") ? parseInt(s, 16) : NaN);
        const [c, ...a] = w;
        const H = (v, n) => v.toString(16).toUpperCase().padStart(n, "0"), one = a.length === 1 && !isNaN(num(a[0]));
        if (c === "info" && !a.length) return `ok v${this.v1 ? 1 : 3} started ${+this.started} held ${+this.held} banks ${H(this.banks.length, 2)}` +
            (this.v1 ? "" : ` kits ${H(this.kits, 2)} ${H(this.nkit, 2)} cap ${H(this.capFixed ?? this.cap(), 5)} used ${H(this.used, 5)} saved ${+this.saved}` +
                            ` prog ${this.prog} size ${H(this.size, 5)}`);
        if (this.v1 && ["st", "save", "kb", "k", "kx", "kits", "kd", "kc"].includes(c)) return "err ?";
        if (c === "st" && !a.length) return this.req ? "ok busy" : "ok " + H(this.res, 1);
        if (c === "save" && !a.length) {
            if (!this.started) return "err not started";
            if (this.req) return "err busy";
            this.req = 1; this.reqArg = 0; return "ok";
        }
        if (c === "kb" && one) return num(a[0]) >= this.nkit ? "err range" : "ok " + H(this.kitBank[num(a[0])], 2);
        if ((c === "k" || c === "kx" || c === "kits") && one) {
            const n = num(a[0]);
            if (!this.held) return "err hold";
            if (c === "kits" ? n > this.nkit : n >= this.kits) return "err range";
            if (this.req) return "err busy";
            if (c === "k") { this.kit.fill(0xFF); this.kitOpen = n + 1; }
            else { this.req = c === "kx" ? 3 : 4; this.reqArg = n; }
            return "ok";
        }
        if (c === "kd" && a.length >= 1 && !isNaN(num(a[0]))) {
            const h = a[1] || "";
            if (/[^0-9A-Fa-f]/.test(h) || h.length % 2) return "err hex";
            if (h.length > 40) return "err range";
            if (!h.length || a.length !== 3 || isNaN(num(a[2]))) return "err ?";
            const d = ln.unhex(h), at = num(a[0]);
            if (!this.held) return "err hold";
            if (!this.kitOpen) return "err kit";
            if (at + d.length > 0x4000) return "err range";
            if (num(a[2]) !== ln.crc16(d)) return "err crc";
            this.kit.set(d, at); return "ok";
        }
        if (c === "kc" && one) {
            if (!this.held) return "err hold";
            if (!this.kitOpen) return "err kit";
            if (this.req) return "err busy";
            if (num(a[0]) !== fs.crc32(this.kit)) return "err crc";
            this.req = 2; this.reqArg = this.kitOpen - 1; this.kitOpen = 0; return "ok";
        }
        if (c === "hold" && !a.length) {
            if (!this.started) return "err not started";
            this.held = true; this.onHold?.(); return "ok";
        }
        if (c === "go" && (!a.length || (a.length === 1 && a[0] === "reset"))) {
            this.held = false; if (a.length && this.started) this.resets++; return "ok";
        }
        if (c === "r" && a.length === 2 && !isNaN(num(a[0])) && !isNaN(num(a[1]))) {
            const [at, n] = a.map(num);
            if (!this.held) return "err hold";
            if (!n || n > 256 || at + n > this.size) return "err range";
            return this.data(this.sram.subarray(at, at + n));
        }
        if (c === "w" && a.length >= 1 && !isNaN(num(a[0]))) {
            const h = a[1] || "";
            if (/[^0-9A-Fa-f]/.test(h) || h.length % 2) return "err hex";
            if (h.length > 40) return "err range";
            if (!h.length || a.length !== 3 || isNaN(num(a[2]))) return "err ?";
            const d = ln.unhex(h), at = num(a[0]);
            if (!this.held) return "err hold";
            if (at + d.length > this.size) return "err range";
            if (num(a[2]) !== ln.crc16(d)) return "err crc";
            if (this.writes === this.failWrite || this.writes >= this.failFrom) { this.writes++; return "err crc"; }   // one bad line / a dead link
            this.writes++;
            this.sram.set(d, at); this.log.push(at); return "ok";
        }
        if (c === "b" && a.length === 3 && a.every(s => !isNaN(num(s)))) {
            const [b, off, n] = a.map(num);
            if (b >= this.banks.length || !n || n > 256 || off + n > 0x4000) return "err range";
            return this.data(Uint8Array.from({ length: n }, (_, i) => this.bankByte(b, off + i)));
        }
        if (c === "bc" && a.length === 1 && !isNaN(num(a[0]))) {
            const b = num(a[0]);
            if (b >= this.banks.length) return "err range";
            return "ok " + fs.crc32(Uint8Array.from({ length: 0x4000 }, (_, i) => this.bankByte(b, i))).toString(16).toUpperCase().padStart(8, "0");
        }
        return "err ?";
    }
    send(bytes) {                                       // the console: echo, and on CR the reply and the prompt
        let outp = "";
        for (const ch of bytes) {
            if (ch !== 13) { this.line += String.fromCharCode(ch); outp += String.fromCharCode(ch); continue; }
            const l = this.line; this.line = "";
            let reply = !l ? "" : !/^ble( |$)/.test(l) ? "? (help)\r\n" :           // an older firmware: `ble` is only the status
                        this.hasLsdj && l.trim() !== "ble" ? this.exec(l.slice(3)) + "\r\n" : "lsdj running error 0 emu_errors 0\r\n";
            if (this.corrupt > 0 && /^ok [0-9A-F]{2}/.test(reply)) { this.corrupt--; reply = reply.replace(/^ok ../, "ok ZZ"); }
            if (this.req && !this.manual) { if (this.slow > 0 && /^ble st/.test(l)) this.slow--; else if (!this.slow) this.runReq(); }   // (the step runs right after the console)
            outp += "\r\n" + reply + "> ";
        }
        if (this.mute) return Promise.resolve();
        for (let i = 0; i < outp.length; i += 7) {      // in small pieces, later, as USB delivers them
            const piece = Uint8Array.from(outp.slice(i, i + 7), c => c.charCodeAt(0));
            if (this.latency) setTimeout(() => this.link.feed(piece), this.latency);   // (a slow link: in order, late)
            else queueMicrotask(() => this.link.feed(piece));
        }
        return Promise.resolve();
    }
}
function simWith(sav, banks = [[0x11], [0x22]]) {
    const sim = new Sim(sav.slice(), banks.map(b => Uint8Array.from(b)));
    sim.started = true;
    sim.link = new ln.Link(b => sim.send(b), 150);
    return sim;
}
function fakePort(sim) {
    const port = { isOpen: false, readerLocked: false, writerLocked: false, ended: false, queue: [], waiting: null };
    const end = () => { port.ended = true; if (port.waiting) { const w = port.waiting; port.waiting = null; w({ value: undefined, done: true }); } };
    sim.link = { feed: bytes => { if (port.waiting) { const w = port.waiting; port.waiting = null; w({ value: bytes, done: false }); } else port.queue.push(bytes); } };
    port.open = async () => { if (port.isOpen) throw new Error("The port is already open."); port.isOpen = true; port.ended = false; port.queue = []; };
    port.setSignals = async () => {};
    port.close = async () => { if (port.readerLocked || port.writerLocked) throw new TypeError("Cannot close: a stream is locked."); port.isOpen = false; };
    port.unplug = end;
    port.readable = { getReader: () => { port.readerLocked = true; return {
        read: () => (port.queue.length ? Promise.resolve({ value: port.queue.shift(), done: false }) : port.ended ? Promise.resolve({ value: undefined, done: true }) : new Promise(r => { port.waiting = r; })),
        cancel: async () => end(),
        releaseLock: () => { port.readerLocked = false; } }; } };
    port.writable = { getWriter: () => { port.writerLocked = true; return { write: b => sim.send(b), releaseLock: () => { port.writerLocked = false; } }; } };
    return port;
}
const rejects = async (p, re) => { try { await p; } catch (e) { return re.test(e.message); } return false; };

/* the firmware's own answers (tests/lsdj/lsdj_xfer_test.c wrote them): the simulated device must give the same */
{
    const V = new URL("../build/tests/lsdj_xfer_vectors.txt", import.meta.url).pathname;
    if (!existsSync(V)) console.log("== skip: no build/tests/lsdj_xfer_vectors.txt (run tests/lsdj/lsdj_xfer_test first)");
    else {
        const sram = Uint8Array.from({ length: 0x20000 }, (_, i) => (i * 7 + (i >> 8)) & 0xFF);
        const sim = new Sim(sram, [0, 1, 2].map(b => Uint8Array.from({ length: 100 * (b + 1) }, (_, j) => (b * 31 + j) & 0xFF)));
        Object.assign(sim, { manual: true, nkit: 3, kitBank: [34, 35, 36], capFixed: 0x1DFE0 });
        let bad = 0, n = 0, cmd = null;
        for (const l of readFileSync(V, "utf8").split("\n")) {
            if (l === "! started") sim.started = true;
            else if (l.startsWith("! flash ")) { const [k, cap, used, saved] = l.slice(8).split(" ").map(x => parseInt(x, 16)); Object.assign(sim, { kits: k, capFixed: cap, used, saved: !!saved }); }
            else if (l.startsWith("! done ")) { sim.req = 0; sim.res = parseInt(l.slice(7), 16); }
            else if (l.startsWith("! prog ")) { const [p, size] = l.slice(7).split(" ").map(x => parseInt(x, 16)); Object.assign(sim, { prog: p, size }); }
            else if (l.startsWith("> ")) cmd = l.slice(2);
            else if (l.startsWith("< ")) { n++; if (sim.exec(cmd) !== l.slice(2)) { bad++; console.log(`   ${cmd} -> ${sim.exec(cmd)}, the firmware: ${l.slice(2)}`); } }
        }
        ok(n > 90 && bad === 0, `the simulated FM-1 answers ${n} commands as the firmware does`);
    }
}

/* ---- the link ---- */
await (async () => {
    ok(ln.crc16(new TextEncoder().encode("123456789")) === 0x31C3, "CRC-16 check value");
    const base = emptySav(); addPacked(base, 2, testSong(4), "DEVSONG", 7);
    let sim = simWith(base);
    await sim.link.sync();
    const info = await sim.link.info();
    ok(info.version === 3 && info.started && !info.held && info.banks === 2 && info.nkit === 0 && info.prog === "lsdj" && info.size === 0x20000, "info");
    ok(await rejects(sim.link.read(0, 4), /hold/), "a read without hold is refused by the device");
    await sim.link.hold();
    ok(eq(await sim.link.read(0x8000, 1000), base.subarray(0x8000, 0x8000 + 1000)), "1000 bytes read in pieces of 256");
    const blob = Uint8Array.from({ length: 200 }, (_, i) => 255 - i);
    await sim.link.write(0x100, blob);
    ok(eq(sim.sram.subarray(0x100, 0x100 + 200), blob) && sim.log.length === 10 && sim.log[1] === 0x114 && sim.log[9] === 0x1B4, "200 bytes written in pieces of 20");
    sim.failWrite = sim.writes;
    await sim.link.write(0x200, blob.subarray(0, 8));
    ok(eq(sim.sram.subarray(0x200, 0x208), blob.subarray(0, 8)), "a write refused once is sent again");
    sim.corrupt = 2;
    ok(eq(await sim.link.read(0x8000, 16), base.subarray(0x8000, 0x8010)), "a damaged reply is asked for again");
    sim.corrupt = 3;
    ok(await rejects(sim.link.read(0x8000, 16), /damaged/), "three damaged replies: the read stops");
    ok((await sim.link.bankCrc(1)) === fs.crc32(Uint8Array.from({ length: 0x4000 }, (_, i) => (i ? 0xFF : 0x22))), "a bank's CRC-32");
    const [r1, r2] = await Promise.all([sim.link.read(0x8000, 8), sim.link.read(0x8100, 8)]);
    ok(eq(r1, base.subarray(0x8000, 0x8008)) && eq(r2, base.subarray(0x8100, 0x8108)), "two commands at once are answered in turn");
    sim.mute = true;
    ok(await rejects(sim.link.info(), /does not answer/), "silence: the FM-1 does not answer");
    sim = simWith(base); sim.hasLsdj = false;
    ok(await rejects(sim.link.info(), /no transfer command/), "a firmware without the command is named");

    /* ---- the page's flows ---- */
    const kit = kitBankData(7), plain = kitBankData(8);
    const withKits = () => simWith(base, Array.from({ length: 14 }, (_, b) => (b === 8 ? [...kit] : b === 9 ? [...plain] : b === 10 ? [0xFF, 0xFF] : [b + 1])));
    sim = withKits();
    const l = await ln.listSongs(sim.link);
    ok(l.list.length === 1 && l.list[0].name === "DEVSONG" && l.free === fs.freeBlocks(base) && !sim.held && sim.resets === 0, "the song list; the Game Boy runs again, not restarted");

    /* Review Focus 2: not started is said plainly */
    sim.started = false;
    ok(await rejects(ln.listSongs(sim.link), /not started/), "not started is said plainly");
    sim.started = true;

    const dl = await ln.downloadSong(sim.link, 2);
    const kitsOnDevice = fs.songKitBanks(base, 2).map(b => Uint8Array.from({ length: 0x4000 }, (_, i) => sim.bankByte(b, i)));
    ok(dl.name === "DEVSONG-v07.lsdprj" && eq(dl.bytes, fs.buildSongFile(base, 2, kitsOnDevice)) && !sim.held && sim.resets === 0, "a song downloads with its kit banks");
    ok(await rejects(ln.downloadSong(sim.link, 9), /empty/) && !sim.held, "an empty slot: said, and the Game Boy runs again");

    /* upload: a song whose two kits are in banks 8 and 9 of this device */
    const src = emptySav(); addPacked(src, 0, testSong(1), "UP", 3);
    const upFile = fs.buildSongFile(src, 0, [kit, plain]);
    const crcs = await ln.bankCrcs(sim.link, 14);
    ok(crcs.length === 14 && crcs[8] === fs.crc32(kit), "the bank CRCs of the device");
    const { slot } = await ln.uploadSong(sim.link, upFile);
    const expect = fs.importSong(base, fs.parseSongFile(upFile), [8, 9]).sav;
    ok(slot === 0 && eq(sim.sram.subarray(0x8000), expect.subarray(0x8000)) && eq(sim.sram.subarray(0, 0x8000), base.subarray(0, 0x8000)), "an upload: the device's file system is the imported one, the edited song untouched");
    ok(sim.resets === 1 && !sim.held, "... and LSDj starts again");
    ok(sim.log.at(-1) >= fs.ACTIVE && sim.log.at(-1) < fs.BLOCKS && sim.log.findIndex(a => a >= fs.ACTIVE && a < fs.BLOCKS) > sim.log.findLastIndex(a => a >= fs.BLOCKS), "the block table is written after every block");

    /* Review Focus 1: a failing write leaves the block table alone */
    sim = withKits(); sim.failFrom = 5;
    ok(await rejects(ln.uploadSong(sim.link, upFile), /crc/) && eq(sim.sram.subarray(fs.ACTIVE, fs.BLOCKS), base.subarray(fs.ACTIVE, fs.BLOCKS)) && !sim.held && sim.resets === 0, "a failing write leaves the block table alone; no restart");

    /* Review Focus 3: a damaged file sends nothing */
    sim = withKits();
    ok(await rejects(Promise.resolve().then(() => ln.uploadSong(sim.link, base.subarray(0, 0x20000))), /song/) && sim.writes === 0 && !sim.held, "a damaged file sends nothing (a .sav dropped as a song)");
    ok(await rejects(Promise.resolve().then(() => ln.uploadSong(sim.link, new Uint8Array(100))), /too short/), "... nor a short file");
    const alien = fs.buildSongFile(src, 0, [kitBankData(99), plain]);
    ok(await rejects(Promise.resolve().then(() => ln.uploadSong(sim.link, alien)), /1 kit/) && sim.writes === 0 && !sim.held, "a kit the FM-1 does not have and no kit bank for it: refused with the count, nothing written");

    /* Review Focus 4: an upload reads the header again */
    sim = withKits();
    await ln.listSongs(sim.link);
    sim.onHold = () => { sim.onHold = null; addPacked(sim.sram, 0, testSong(6), "LATER", 1); };   // saved in LSDj meanwhile
    ok((await ln.uploadSong(sim.link, upFile)).slot === 1 && fs.songs(sim.sram).map(s => s.name).join() === "LATER,UP,DEVSONG" && eq(fs.unpackSong(sim.sram, 0), testSong(6)), "an upload reads the header again: a song saved meanwhile is kept");

    sim = withKits();
    await ln.deleteSong(sim.link, 2);
    ok(fs.songs(sim.sram).length === 0 && sim.resets === 1, "a song is deleted");

    sim = withKits();
    const whole = await ln.downloadSav(sim.link);
    ok(eq(whole, base) && !sim.held && sim.resets === 0, "the whole save downloads");
    const other = emptySav(); addPacked(other, 1, testSong(2), "NEW", 1); other[5] = 0x99;
    await ln.uploadSav(sim.link, other);
    ok(eq(sim.sram, other) && sim.resets === 1, "a save is restored; only changed pieces are written");
    ok(sim.log.length > 0 && sim.log.length < 1500, "... the changed pieces, not all 6554");
    ok(await rejects(Promise.resolve().then(() => ln.uploadSav(sim.link, other.subarray(0, 0x8000))), /131072/), "a save of the wrong size is refused");
    const blank = new Uint8Array(fs.SAV_SIZE);
    ok(await rejects(Promise.resolve().then(() => ln.uploadSav(sim.link, blank)), /file system mark/), "a file that is not an LSDj save is refused");
    { // DMGo installed (info: prog 1, size 08000): its whole save in and out, nothing of LSDj's file system
        const d = Uint8Array.from({ length: 0x8000 }, (_, i) => (i * 13) & 0xFF);
        let s = simWith(d);
        s.prog = 1;
        const inf = await s.link.info();
        ok(inf.prog === "dmgo" && inf.size === 0x8000, "DMGo: info says so, and its save's size");
        const got = await ln.downloadSav(s.link);
        ok(got.length === 0x8000 && eq(got, d) && !s.held, "DMGo: its 32 KiB save downloads");
        const d2 = d.slice(); d2[100] ^= 0xFF; d2[0x7FFF] ^= 1;
        await ln.uploadSav(s.link, d2);
        ok(eq(s.sram, d2) && s.resets === 1, "DMGo: a 32 KiB save is restored (no file system mark needed)");
        ok(await rejects(ln.uploadSav(s.link, base), /runs DMGo.*32768/) && eq(s.sram, d2) && !s.held, "DMGo: an LSDj save is refused, nothing written");
        ok(await rejects(Promise.resolve().then(() => ln.uploadSav(s.link, new Uint8Array(1000))), /131072 .*32768/),
           "a file of neither size is refused");
        s = simWith(base);
        ok(await rejects(ln.uploadSav(s.link, d2), /runs LSDj.*131072/) && eq(s.sram, base) && !s.held, "LSDj: a DMGo save is refused, nothing written");
    }

    /* ---- after the final review (2026-10-12) ---- */
    /* a reply belongs to its command: the console's own banner (printed when the port opens) on a slow link */
    sim = simWith(base); sim.latency = 3;
    setTimeout(() => sim.link.feed(Uint8Array.from("\r\nLSDj FM-1 lsdj-9 console - 'help'\r\n> ", c => c.charCodeAt(0))), 1);
    await sim.link.sync();
    const afterBanner = await sim.link.info().catch(e => e);
    ok(afterBanner.started === true, "the console's banner at connect is not taken for a reply");
    sim = simWith(base); await sim.link.hold(); sim.latency = 200;                 // slower than this link's 150 ms patience
    ok(await rejects(sim.link.cmd("r 8000 4"), /does not answer/), "(a reply later than the link waits)");
    sim.latency = 80;                                                              // the late reply lands while the next command waits
    ok((await sim.link.cmd("r 8100 4").catch(e => e.message)) === sim.data(base.subarray(0x8100, 0x8104)).slice(3), "a late reply is not taken for the next command's");

    /* a failed connect closes the port, so the next one works (Web Serial refuses to close a locked port) */
    const dev = new Sim(base.slice(), [Uint8Array.of(1)]); dev.started = true; dev.hasLsdj = false;
    const port = fakePort(dev);
    let lost = 0;
    ok(await rejects(ln.openPort(port, () => lost++, 150), /no transfer command/) && !port.isOpen && !port.readerLocked && !port.writerLocked, "a failed connect leaves the port closed");
    dev.hasLsdj = true;
    const conn = await ln.openPort(port, () => lost++, 150).catch(e => e);
    ok(conn.info && conn.info.started && conn.info.banks === 1 && port.isOpen, "... so the next connect works");
    if (conn.close) await conn.close();
    ok(!port.isOpen && lost === 0, "closing is not reported as a lost FM-1");
    await ln.openPort(port, () => lost++, 150);
    port.unplug();
    await new Promise(r => setTimeout(r, 5));
    ok(lost === 1 && !port.isOpen, "an unplugged FM-1 is reported once and the port closed");

    /* Delete and Download act on the song the list showed, not on whatever is in that slot now */
    sim = withKits();
    const shown = (await ln.listSongs(sim.link)).list[0];                          // DEVSONG in slot 2
    const resave = () => { sim.onHold = null; sim.sram.set(fs.removeSong(sim.sram, 2)); addPacked(sim.sram, 2, testSong(8), "NEWTRACK", 9); };
    sim.onHold = resave;                                                           // re-saved in LSDj meanwhile
    ok(await rejects(ln.deleteSong(sim.link, shown.slot, shown), /list has changed/) && fs.songs(sim.sram)[0].name === "NEWTRACK" && sim.resets === 0 && !sim.held, "Delete on a row that changed removes nothing");
    ok(await rejects(ln.downloadSong(sim.link, shown.slot, undefined, shown), /list has changed/) && !sim.held, "... and Download does not hand over another song under the old name");
    const now = (await ln.listSongs(sim.link)).list[0];
    await ln.deleteSong(sim.link, now.slot, now);
    ok(fs.songs(sim.sram).length === 0, "the song the list shows is deleted");

    /* a restore writes the file system's header last, and says so when it could not finish */
    sim = withKits();
    await ln.uploadSav(sim.link, other);
    const inHead = a => a >= 0x8000 && a < fs.BLOCKS;
    ok(eq(sim.sram, other) && sim.log.some(inHead) && sim.log.findIndex(inHead) > sim.log.findLastIndex(a => !inHead(a)), "a restore writes the names and the block table after everything else");
    sim = withKits(); sim.failFrom = 3;
    ok(await rejects(ln.uploadSav(sim.link, other), /incomplete: restore it again/) && !sim.held && sim.resets === 0 && eq(sim.sram.subarray(0x8000, fs.BLOCKS), base.subarray(0x8000, fs.BLOCKS)), "a restore that stops says the save is incomplete; the names and block table are still the old ones");

    /* ---- saving to flash and kit banks (protocol 2) ---- */
    const flashSim = (k = 0) => {                       // 40 ROM banks: kits in 8 and 9, the free kit banks 34..36
        const s = simWith(base, Array.from({ length: 40 }, (_, b) => (b === 8 ? [...kit] : b === 9 ? [...plain] : b >= 34 ? [] : [b + 1])));
        Object.assign(s, { nkit: 3, kitBank: [34, 35, 36], kits: k });
        return s;
    };
    sim = flashSim();
    const inf = await sim.link.info();
    ok(inf.version === 3 && inf.kits === 0 && inf.nkit === 3 && inf.cap === 29 * 4096 - 256 && inf.used === 0 && !inf.saved && inf.flash, "info: the flash store");
    await ln.saveToFlash(sim.link);
    ok(sim.saved && sim.saves === 1 && eq(sim.flashImage, sim.sram) && !sim.held, "save now");
    sim.slow = 3;
    await ln.saveToFlash(sim.link);
    ok(sim.saves === 2, "a save that takes a while is waited for");
    sim.fullOver = 10;
    ok(await rejects(ln.saveToFlash(sim.link), /does not fit/) && sim.saves === 2, "a save that does not fit is said so");

    /* every change the page makes is saved to flash */
    sim = flashSim();
    const up2 = await ln.uploadSong(sim.link, upFile);
    ok(up2.slot === 0 && up2.unsaved === null && sim.saves === 1 && eq(sim.flashImage, sim.sram) && sim.resets === 1, "an upload is saved to flash");
    await ln.deleteSong(sim.link, 0);
    ok(sim.saves === 2 && fs.songs(sim.flashImage).length === 1, "a delete is saved to flash");
    await ln.uploadSav(sim.link, other);
    ok(sim.saves === 3 && eq(sim.flashImage, other), "a restore is saved to flash");
    sim = flashSim(); sim.fullOver = 10;
    const up3 = await ln.uploadSong(sim.link, upFile);
    ok(up3.slot === 0 && /does not fit/.test(up3.unsaved) && fs.songs(sim.sram).length === 2 && sim.resets === 1 && !sim.saved, "an upload that cannot be saved is still there, and said to be unsaved");

    /* a song's kits travel into flash kit banks */
    const kitA = kitBankData(0x41), kitB = kitBankData(0x42);   // not on the device
    kitA.set([0x4B, 0x49, 0x54, 0x2D, 0x41, 0x20], 0x52);       // its name: "KIT-A "
    const custom = fs.buildSongFile(src, 0, [kitA, plain]);     // the song's kits 3 and 5: kitA, and one the ROM has
    sim = flashSim(1);                                          // one flash kit bank, free
    const up4 = await ln.uploadSong(sim.link, custom);
    ok(eq(sim.banks[34], kitA) && sim.kits === 1, "a kit the FM-1 lacks goes into a free flash kit bank");
    const got4 = fs.unpackSong(sim.sram, up4.slot);
    ok(got4[0x3082] === (0x80 | fs.bankKit(34)) && got4[0x3089] === fs.bankKit(9), "... and the song names it there");
    ok(sim.saves === 1 && sim.resets === 1 && !sim.held && up4.unsaved === null, "... saved, and LSDj started again");
    const again4 = await ln.uploadSong(sim.link, custom);
    ok(again4.slot !== up4.slot && sim.kits === 1 && eq(sim.banks[34], kitA), "the same kit again is found there, not stored twice");
    const kl = await ln.listKits(sim.link);
    ok(kl.length === 1 && kl[0].j === 0 && kl[0].bank === 34 && kl[0].name === "KIT-A" && !kl[0].free, "the kit list: the bank and the kit's name");

    sim = flashSim(0);                                          // no kit bank yet: the FM-1 makes one
    const up5 = await ln.uploadSong(sim.link, custom);
    ok(sim.kits === 1 && eq(sim.banks[34], kitA) && up5.unsaved === null && fs.songs(sim.sram).length === 2, "no free kit bank: one more is made, and the kit goes into it");
    sim = flashSim(3); sim.banks[34] = kitBankData(1); sim.banks[35] = kitBankData(2); sim.banks[36] = kitBankData(3);
    ok(await rejects(ln.uploadSong(sim.link, custom), /no room for another kit/) && fs.songs(sim.sram).length === 1 && !sim.held, "every kit bank taken: refused, nothing changed");
    const twoKits = fs.buildSongFile(src, 0, [kitA, kitB]);
    sim = flashSim(1);
    await ln.uploadSong(sim.link, twoKits);
    ok(sim.kits === 2 && eq(sim.banks[34], kitA) && eq(sim.banks[35], kitB), "two kits, one free bank: one more bank, both stored");
    sim = flashSim(0); sim.noBank = true;                       // (a save that leaves no room for a bank)
    ok(await rejects(ln.uploadSong(sim.link, custom), /would no longer fit/) && fs.songs(sim.sram).length === 1 && sim.kits === 0 && !sim.held, "no room for a kit bank beside the save: said, and the song is not added");

    sim = flashSim(2); sim.banks[34] = kitA.slice();
    await ln.removeKit(sim.link, 0);
    ok(sim.banks[34].length === 0 && sim.resets === 1 && (await ln.listKits(sim.link))[0].free, "a kit is removed: its bank is free");
    await ln.setKitBanks(sim.link, 0);
    ok(sim.kits === 0 && sim.resets === 2, "unused kit banks go back to the save");
    sim = flashSim(1);                                          // a removal the flash reports as failed, the bank already erased
    await ln.uploadSong(sim.link, custom);                      // (the page now knows bank 34 holds kitA)
    sim.failReq = 2;
    ok(await rejects(ln.removeKit(sim.link, 0), /did not take it/) && sim.banks[34].length === 0, "a removal that fails is said");
    sim.failReq = 0;
    await ln.uploadSong(sim.link, custom);
    ok(eq(sim.banks[34], kitA), "... and the kit is not believed to be there still: the next song brings it again");

    /* an FM-1 with the firmware before saving (protocol 1): everything but flash still works */
    sim = withKits(); sim.v1 = true;
    const inf1 = await sim.link.info();
    const up6 = await ln.uploadSong(sim.link, upFile);
    ok(inf1.version === 1 && !inf1.flash && up6.slot === 0 && /cannot save/.test(up6.unsaved) && sim.saves === 0 && sim.resets === 1, "the firmware before saving: uploads work, and unsaved is said");
})();

/* ---- the page, inlined as web/make_site.py does: one script that compiles ---- */
{
    const HERE = new URL(".", import.meta.url).pathname;
    if (!existsSync(HERE + "lsdj.html")) console.log("== skip: no web/lsdj.html yet");
    else {
        const strip = s => s.replace(/^export\s+/gm, "").replace(/^import .*?;\n/gm, "");
        const html = readFileSync(HERE + "lsdj.html", "utf8");
        const lib = strip(readFileSync(HERE + "lsdjfs.js", "utf8")) + "\n" + strip(readFileSync(HERE + "lsdjlink.js", "utf8"));
        const m = /<script>([\s\S]*)<\/script>/.exec(html.replace("/*LIB*/", lib));
        let compiled = false;
        try { new vm.Script(m[1]); compiled = true; } catch (e) { console.log("   " + e.message); }
        ok(html.split("/*LIB*/").length === 2 && compiled && !/^(import|export) /m.test(m[1]), "lsdj.html with the modules inlined is one script that compiles");
    }
}

console.log(failed ? `test_lsdj: ${failed} FAILED` : "test_lsdj: all passed");
process.exit(failed ? 1 : 0);
