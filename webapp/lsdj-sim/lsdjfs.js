// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// LSDj's save file system and song files, with no browser and no device. The rules are LSDPatcher's (Johan
// Kotlinski, MIT: LSDSavFile.java), written anew.
//   the save (128 KiB): song names at 0x8000 (32 x 8), versions at 0x8100, the active slot at 0x8140, the block
//   table at 0x8141 (191 blocks: the slot that owns each, 0xFF free), blocks of 512 B from 0x8200
//   a song file (.lsdsng; .lsdprj with kits): 8 name bytes, 1 version byte, the song's blocks in order, then one
//   16 KiB ROM bank per kit the song uses, in rising kit order
//   packing: C0 C0 = C0, C0 b n = n times b, E0 E0 = E0, E0 F0 n = n default waves, E0 F1 n = n default
//   instruments, E0 FF = the end, E0 k = go on in block k (counted from 1: 0x8000 + 0x200 * k)
export const SAV_SIZE = 0x20000, BLOCK = 0x200, BANK = 0x4000, SLOTS = 32, NBLOCKS = 0xBF;
export const NAMES = 0x8000, VERSIONS = 0x8100, ACTIVE = 0x8140, FAT = 0x8141, BLOCKS = 0x8200;
const FS_BASE = 0x8000, SONG_SIZE = 0x8000, INSTR = 0x3080, NINSTR = 0x40;
const DEF_WAVE = [0x8E, 0xCD, 0xCC, 0xBB, 0xAA, 0xA9, 0x99, 0x88, 0x87, 0x76, 0x66, 0x55, 0x54, 0x43, 0x32, 0x31];
const DEF_INSTR = [0xA8, 0, 0, 0xFF, 0, 0, 3, 0, 0, 0xD0, 0, 0, 0, 0xF3, 0, 0];

/* kits live in ROM banks 8..26 and 32..63 */
export const kitBank = kit => (kit + 8 > 26 ? kit + 13 : kit + 8);
export const bankKit = bank => (bank > 26 ? bank - 13 : bank - 8);

export function slotBlocks(sav, slot) {
    const out = [];
    for (let i = 0; i < NBLOCKS; i++) if (sav[FAT + i] === slot) out.push(i);
    return out;
}

export function freeBlocks(sav) {
    let n = 0;
    for (let i = 0; i < NBLOCKS; i++) if (sav[FAT + i] >= SLOTS) n++;
    return n;
}

export function nameText(sav, slot) {
    let s = "";
    for (let i = 0; i < 8; i++) {
        const c = sav[NAMES + 8 * slot + i];
        if (!c) break;
        s += (c >= 65 && c <= 90) || (c >= 48 && c <= 57) ? String.fromCharCode(c) : " ";
    }
    return s;
}

export function songs(sav) {
    const out = [];
    for (let slot = 0; slot < SLOTS; slot++) {
        const blocks = slotBlocks(sav, slot).length;
        if (blocks) out.push({ slot, name: nameText(sav, slot), version: sav[VERSIONS + slot], blocks });
    }
    return out;
}

/* a slot's song: { song: its 32 KiB, src: for each song byte the save address it was copied from, -1 if it came
   from a run or a default }; null for an empty slot or a damaged song */
export function decodeSong(sav, slot) {
    const first = slotBlocks(sav, slot)[0];
    if (first === undefined) return null;
    const song = new Uint8Array(SONG_SIZE), src = new Int32Array(SONG_SIZE).fill(-1);
    let p = BLOCKS + BLOCK * first, o = 0, jumps = 0;
    const put = (v, from) => { if (o >= SONG_SIZE) return false; song[o] = v; src[o++] = from; return true; };
    const fill = (pat, count) => {
        for (let k = 0; k < count * pat.length; k++) if (!put(pat[k % pat.length], -1)) return false;
        return true;
    };
    for (;;) {
        if (p + 2 >= sav.length) return null;
        const c = sav[p], n = sav[p + 1];
        if (c === 0xC0) {
            if (n === 0xC0) { if (!put(0xC0, -1)) return null; p += 2; }
            else { if (!fill([n], sav[p + 2])) return null; p += 3; }
        } else if (c === 0xE0) {
            if (n === 0xE0) { if (!put(0xE0, -1)) return null; p += 2; }
            else if (n === 0xFF) return o === SONG_SIZE ? { song, src } : null;
            else if (n === 0xF0) { if (!fill(DEF_WAVE, sav[p + 2])) return null; p += 3; }
            else if (n === 0xF1) { if (!fill(DEF_INSTR, sav[p + 2])) return null; p += 3; }
            else {
                if (n < 1 || n > NBLOCKS || ++jumps > NBLOCKS) return null;
                p = FS_BASE + BLOCK * n;
            }
        } else {
            if (!put(c, p)) return null;
            p += 1;
        }
    }
}

export function unpackSong(sav, slot) {
    const d = decodeSong(sav, slot);
    return d ? d.song : null;
}

/* kit numbers of the song's kit instruments (type 2: bytes 2 and 9, low 6 bits), rising */
export function usedKits(song) {
    const kits = new Set();
    for (let i = 0; i < NINSTR; i++) {
        const p = INSTR + 16 * i;
        if (song[p] !== 2) continue;
        kits.add(song[p + 2] & 0x3F);
        kits.add(song[p + 9] & 0x3F);
    }
    return [...kits].sort((a, b) => a - b);
}

/* the ROM banks a slot's song needs with it, in the order a song file carries them */
export function songKitBanks(sav, slot) {
    const song = unpackSong(sav, slot);
    if (!song) throw new Error("the song is damaged");
    return usedKits(song).map(kitBank);
}

export function buildSongFile(sav, slot, kits) {
    const blocks = slotBlocks(sav, slot);
    if (!blocks.length) throw new Error("the slot is empty");
    const out = new Uint8Array(9 + BLOCK * blocks.length + BANK * kits.length);
    out.set(sav.subarray(NAMES + 8 * slot, NAMES + 8 * slot + 8), 0);
    out[8] = sav[VERSIONS + slot];
    blocks.forEach((b, i) => out.set(sav.subarray(BLOCKS + BLOCK * b, BLOCKS + BLOCK * (b + 1)), 9 + BLOCK * i));
    kits.forEach((k, i) => {
        if (k.length !== BANK) throw new Error("a kit bank is not 16 KiB");
        out.set(k, 9 + BLOCK * blocks.length + BANK * i);
    });
    return out;
}

export function songFileName(sav, slot) {
    const name = nameText(sav, slot).trim().replace(/ /g, "_") || "SONG";
    return `${name}-v${sav[VERSIONS + slot].toString(16).toUpperCase().padStart(2, "0")}.lsdprj`;
}

/* in a block: the offset of the byte that names the next block, or -1 at the end mark */
export function nextBlockPtr(b) {
    for (let i = 0; i + 1 < BLOCK; i++) {
        if (b[i] === 0xC0) {
            i++;
            if (b[i] !== 0xC0) i++;
        } else if (b[i] === 0xE0) {
            const n = b[i + 1];
            if (n === 0xE0) i++;
            else if (n === 0xFF) return -1;
            else if (n === 0xF0 || n === 0xF1) i += 2;
            else return i + 1;
        }
    }
    throw new Error("song corrupted: a block without a jump or an end");
}

export function parseSongFile(bytes) {
    if (bytes.length < 9 + BLOCK) throw new Error("not a song file: too short");
    const blocks = [];
    let p = 9;
    for (;;) {
        if (p + BLOCK > bytes.length) throw new Error("song corrupted: the file ends inside its blocks");
        if (blocks.length === NBLOCKS) throw new Error("song corrupted: more blocks than a save holds");
        const b = bytes.slice(p, p + BLOCK);
        blocks.push(b);
        p += BLOCK;
        if (nextBlockPtr(b) < 0) break;
    }
    if ((bytes.length - p) % BANK) throw new Error("not a song file: the kit data is not whole 16 KiB banks");
    const kits = [];
    for (; p < bytes.length; p += BANK) kits.push(bytes.slice(p, p + BANK));
    return { name: bytes.slice(0, 8), version: bytes[8], blocks, kits };
}

/* a copy of the save with the song in its first free slot. kitBanks[i]: the ROM bank that holds file.kits[i] where
   the save lives (an .lsdsng brings none and keeps its kit numbers). Throws, changing nothing, when the song does
   not fit, is damaged or cannot be renumbered. */
export function importSong(sav, file, kitBanks = []) {
    const out = sav.slice();
    let slot = 0;
    while (slot < SLOTS && slotBlocks(out, slot).length) slot++;
    if (slot === SLOTS) throw new Error("no free song slot: all 32 are used");
    if (file.blocks.length > freeBlocks(out))
        throw new Error(`not enough room: the song needs ${file.blocks.length} blocks, ${freeBlocks(out)} are free`);
    out.fill(0, NAMES + 8 * slot, NAMES + 8 * slot + 8);
    out.set(file.name.subarray(0, 8), NAMES + 8 * slot);
    out[VERSIONS + slot] = file.version;
    let prev = -1;
    for (const b of file.blocks) {
        let id = 0;
        while (out[FAT + id] < SLOTS) id++;
        if (prev >= 0) out[prev] = id + 1;
        out[FAT + id] = slot;
        out.set(b, BLOCKS + BLOCK * id);
        const j = nextBlockPtr(b);
        prev = j < 0 ? -1 : BLOCKS + BLOCK * id + j;
    }
    if (prev >= 0) throw new Error("song corrupted: its last block has no end mark");
    const d = decodeSong(out, slot);
    if (!d) throw new Error("song corrupted: it does not unpack to a whole song");
    if (!file.kits.length) return { sav: out, slot };
    const old = usedKits(d.song);
    if (old.length !== file.kits.length || kitBanks.length !== file.kits.length)
        throw new Error(`the file carries ${file.kits.length} kits, the song uses ${old.length}`);
    const map = new Map(old.map((k, i) => [k, bankKit(kitBanks[i])]));
    for (let i = 0; i < NINSTR; i++) {
        const p = INSTR + 16 * i;
        if (d.song[p] !== 2) continue;
        for (const q of [p + 2, p + 9]) {
            const v = d.song[q], k = map.get(v & 0x3F), nv = (v & 0xC0) | k;
            if (nv === v) continue;
            if (d.src[q] < 0 || nv === 0xC0 || nv === 0xE0)   // inside a run, or the new byte would be an escape
                throw new Error("this song's kit numbers cannot be renumbered for this FM-1");
            out[d.src[q]] = nv;
        }
    }
    return { sav: out, slot };
}

export function removeSong(sav, slot) {
    const out = sav.slice();
    for (let i = 0; i < NBLOCKS; i++) if (out[FAT + i] === slot) out[FAT + i] = 0xFF;
    out[NAMES + 8 * slot] = 0;
    out[VERSIONS + slot] = 0;
    if (out[ACTIVE] === slot) out[ACTIVE] = 0xFF;
    return out;
}

let crcTable = null;
export function crc32(bytes) {
    if (!crcTable) {
        crcTable = new Uint32Array(256);
        for (let n = 0; n < 256; n++) {
            let c = n;
            for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
            crcTable[n] = c >>> 0;
        }
    }
    let c = 0xFFFFFFFF;
    for (const b of bytes) c = crcTable[(c ^ b) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
}
