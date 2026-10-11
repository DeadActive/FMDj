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

console.log(failed ? `test_lsdj: ${failed} FAILED` : "test_lsdj: all passed");
process.exit(failed ? 1 : 0);
