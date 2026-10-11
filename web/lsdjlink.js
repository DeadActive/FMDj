// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// The LSDj build's transfer commands (the console's `ble WORDS`, firmware/src/lsdj/lsdj_xfer.c) from a page, and
// what the song page does with them. No browser API here: the page gives Link a function that sends bytes and feeds it what arrives.
//   a command: "ble WORDS\r" -> the console echoes it, then "\r\n", one line "ok ..." or "err ...", "\r\n> "
//   (the console line is 64 characters: a write carries 20 bytes)
//   the save RAM is read and written only while the Game Boy is held; every flow ends with `go`
import { ACTIVE, BANK, BLOCK, BLOCKS, FAT, NBLOCKS, SAV_SIZE, SLOTS, VERSIONS, buildSongFile, crc32, freeBlocks, importSong, nameText, parseSongFile, removeSong, slotBlocks, songFileName, songKitBanks, songs } from "./lsdjfs.js";

const RD = 256, WR = 20, TRIES = 3, FS_HEAD = 0x8000;
const hx = (v, digits = 1) => v.toString(16).toUpperCase().padStart(digits, "0");
const ascii = s => Uint8Array.from(s, c => c.charCodeAt(0));

export function hexBytes(bytes) {
    let s = "";
    for (const b of bytes) s += hx(b, 2);
    return s;
}

export function unhex(s) {
    if (s.length % 2 || /[^0-9A-Fa-f]/.test(s)) return null;
    const out = new Uint8Array(s.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(2 * i, 2), 16);
    return out;
}

export function crc16(bytes) {                           // XMODEM: 0x1021, start 0
    let c = 0;
    for (const b of bytes) {
        c ^= b << 8;
        for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    }
    return c;
}

export class Link {
    constructor(send, timeoutMs = 2000) {
        Object.assign(this, { send, timeoutMs, text: "", wake: null, queue: Promise.resolve(), stamp: 0 });
    }

    feed(bytes) {                                        // what arrived from the FM-1
        for (const b of bytes) this.text += String.fromCharCode(b);
        this.stamp++;
        if (this.wake) this.wake();
    }

    until(found, ms = this.timeoutMs) {                  // found(text) gives something, or the time is up
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => { this.wake = null; reject(new Error("the FM-1 does not answer")); }, ms);
            this.wake = () => {
                const m = found(this.text);
                if (m) { clearTimeout(timer); this.wake = null; resolve(m); }
            };
            this.wake();
        });
    }

    /* an empty line: the console answers with its prompt. Then wait until nothing more arrives: the console also
       prints a banner and a prompt when the port opens, and that must not be read as a reply */
    async sync(quietMs = 40) {
        this.text = "";
        await this.send(ascii("\r"));
        await this.until(t => /> $/.test(t));
        for (let seen = -1; seen !== this.stamp;) {
            seen = this.stamp;
            await new Promise(r => setTimeout(r, quietMs));
        }
    }

    /* one command -> what follows "ok" in its reply; "err ..." and silence throw. Commands run one at a time.
       A reply counts only after the console's echo of this very line: text that was still on its way (the banner,
       the reply to a command that had timed out) is not this command's answer. */
    cmd(line, ms = this.timeoutMs) {
        const run = async () => {
            const echo = `ble ${line}\r\n`;
            this.text = "";
            await this.send(ascii(`ble ${line}\r`));
            const reply = await this.until(t => {
                const i = t.lastIndexOf(echo);
                return i >= 0 && /\r\n> $/.test(t.slice(i + echo.length - 2)) ? t.slice(i + echo.length) : null;
            }, ms);
            const m = /^(ok|err)(?: ([^\r\n]*))?\r\n> $/.exec(reply);
            if (!m) throw new Error("this FM-1's firmware has no transfer command: install the LSDj build that has it");
            if (m[1] === "err") throw new Error("FM-1: " + (m[2] || "error"));
            return m[2] || "";
        };
        const p = this.queue.then(run, run);
        this.queue = p.catch(() => {});
        return p;
    }

    /* flash: the store's side (protocol 2); an FM-1 with the firmware before saving answers protocol 1 */
    async info() {
        const m = /^v([0-9A-F]+) started ([01]) held ([01]) banks ([0-9A-F]+)(?: kits ([0-9A-F]+) ([0-9A-F]+) cap ([0-9A-F]+) used ([0-9A-F]+) saved ([01]))?$/.exec(await this.cmd("info"));
        if (!m) throw new Error("FM-1: an info reply this page does not know");
        const h = t => parseInt(t || "0", 16);
        return { version: h(m[1]), started: m[2] === "1", held: m[3] === "1", banks: h(m[4]), kits: h(m[5]), nkit: h(m[6]),
                 cap: h(m[7]), used: h(m[8]), saved: m[9] === "1", flash: h(m[1]) >= 2 && h(m[7]) > 0 };
    }

    hold() { return this.cmd("hold"); }
    go(reset = false) { return this.cmd(reset ? "go reset" : "go"); }

    async data(line, n) {                                // a reply "HEX CRC": checked, asked again when damaged
        let last;
        for (let t = 0; t < TRIES; t++) {
            try {
                const [h, c] = (await this.cmd(line)).split(" ");
                const d = unhex(h || "");
                if (d && d.length === n && parseInt(c, 16) === crc16(d)) return d;
                last = new Error("the FM-1 sent damaged data");
            } catch (e) {
                if (!/does not answer/.test(e.message)) throw e;
                last = e;
                await this.sync().catch(() => {});
            }
        }
        throw last;
    }

    async read(addr, len, progress = () => {}) {         // the save RAM (held)
        const out = new Uint8Array(len);
        for (let o = 0; o < len; o += RD) {
            const n = Math.min(RD, len - o);
            out.set(await this.data(`r ${hx(addr + o)} ${hx(n)}`, n), o);
            progress(o + n, len);
        }
        return out;
    }

    async piece(line) {                                  // a line that carries data: sent again on a bad CRC
        for (let t = 0; ; t++) {
            try { await this.cmd(line); return; } catch (e) {
                if (t + 1 === TRIES || !/FM-1: (crc|hex)|does not answer/.test(e.message)) throw e;
                if (/does not answer/.test(e.message)) await this.sync().catch(() => {});
            }
        }
    }

    async write(addr, bytes, progress = () => {}) {      // the save RAM (held)
        for (let o = 0; o < bytes.length; o += WR) {
            const d = bytes.subarray(o, o + WR);
            await this.piece(`w ${hx(addr + o)} ${hexBytes(d)} ${hx(crc16(d), 4)}`);
            progress(Math.min(o + WR, bytes.length), bytes.length);
        }
    }

    /* a flash request (save, kc, kx, kits): the FM-1 queues it and its Game Boy step carries it out, which can take
       seconds with nothing else answered; `st` then gives the result. full: what "it does not fit" means here. */
    async flash(line, full = "it does not fit in the FM-1's flash") {
        await this.cmd(line);
        for (let n = 0; n < 500; n++) {
            const r = await this.cmd("st", Math.max(this.timeoutMs, 30000));
            if (r !== "busy") {
                const code = parseInt(r, 16);
                if (!code) return;
                throw new Error(code === 1 ? full : code === 2 ? "the FM-1's flash did not take it" : "the FM-1 has no such kit bank");
            }
            await new Promise(res => setTimeout(res, 20));
        }
        throw new Error("the FM-1 is still busy");
    }

    async kitBankNo(j) { return parseInt(await this.cmd("kb " + hx(j)), 16); }   // the ROM bank flash kit bank j shows as

    async writeKit(j, bytes, progress = () => {}) {      // a 16 KiB kit bank into flash kit bank j (held)
        await this.cmd("k " + hx(j));
        for (let o = 0; o < BANK; o += WR) {
            const d = bytes.subarray(o, Math.min(o + WR, BANK));
            if (!d.every(v => v === 0xFF)) await this.piece(`kd ${hx(o)} ${hexBytes(d)} ${hx(crc16(d), 4)}`);   // (the buffer starts as FF)
            progress(Math.min(o + WR, BANK), BANK);
        }
        await this.flash("kc " + hx(crc32(bytes), 8));
    }

    async readBank(bank, progress = () => {}) {          // a whole 16 KiB ROM bank
        const out = new Uint8Array(BANK);
        for (let o = 0; o < BANK; o += RD) {
            out.set(await this.data(`b ${hx(bank)} ${hx(o)} ${hx(RD)}`, RD), o);
            progress(o + RD, BANK);
        }
        return out;
    }

    async bankCrc(bank) { return parseInt(await this.cmd("bc " + hx(bank)), 16); }
}

/* the Game Boy held while fn runs; it always runs again after, restarted if fn changed the save and succeeded */
async function whileHeld(link, restart, fn) {
    await link.hold();
    let done = false;
    try {
        const r = await fn();
        done = true;
        return r;
    } finally {
        await link.go(done && restart).catch(() => {});
    }
}

async function fsHeader(link) {                          // a save image with only the file system's first 512 B read
    const sav = new Uint8Array(SAV_SIZE);
    sav.set(await link.read(FS_HEAD, BLOCK), FS_HEAD);
    return sav;
}

const samePiece = (a, b, o, n) => { for (let i = o; i < o + n; i++) if (a[i] !== b[i]) return false; return true; };

/* old -> sav on the device: the blocks that got an owner whole, then the changed pieces of the names and versions,
   the active slot and the block table last (until then LSDj's file system is the old one) */
async function writeChange(link, old, sav, progress = () => {}) {
    const pieces = [];
    for (let b = 0; b < NBLOCKS; b++)
        if (sav[FAT + b] !== old[FAT + b] && sav[FAT + b] < SLOTS) pieces.push([BLOCKS + BLOCK * b, BLOCK]);
    for (const [a, e] of [[FS_HEAD, ACTIVE], [ACTIVE, BLOCKS]])
        for (let o = a; o < e; o += WR) {
            const n = Math.min(WR, e - o);
            if (!samePiece(old, sav, o, n)) pieces.push([o, n]);
        }
    let i = 0;
    for (const [o, n] of pieces) {
        await link.write(o, sav.subarray(o, o + n));
        progress(++i, pieces.length);
    }
}

/* shown: the song as the page's list has it ({ name, version, blocks }). Between drawing the list and a click, LSDj
   may have saved another song into the slot: then nothing is done */
function checkShown(sav, slot, shown) {
    if (shown && (nameText(sav, slot) !== shown.name || sav[VERSIONS + slot] !== shown.version || slotBlocks(sav, slot).length !== shown.blocks))
        throw new Error("the list has changed on the FM-1: press Refresh and try again");
}

export function listSongs(link) {
    return whileHeld(link, false, async () => {
        const sav = await fsHeader(link);
        return { sav, list: songs(sav), free: freeBlocks(sav) };
    });
}

export function downloadSong(link, slot, progress = () => {}, shown = null) {
    return whileHeld(link, false, async () => {
        const sav = await fsHeader(link), blocks = slotBlocks(sav, slot);
        checkShown(sav, slot, shown);
        if (!blocks.length) throw new Error("the slot is empty");
        for (const b of blocks) sav.set(await link.read(BLOCKS + BLOCK * b, BLOCK), BLOCKS + BLOCK * b);
        const banks = songKitBanks(sav, slot), kits = [];
        for (const [i, bank] of banks.entries()) kits.push(await link.readBank(bank, (a, n) => progress(i * n + a, banks.length * n)));
        return { name: songFileName(sav, slot), bytes: buildSongFile(sav, slot, kits) };
    });
}

const SAVE_FULL = "the save does not fit in the FM-1's flash (delete songs or remove a kit)";

/* after a change in the save RAM: into flash, so it survives power-off. -> null, or why it is not saved */
async function persist(link, inf) {
    if (!inf.flash) return "this FM-1's firmware cannot save to flash: it is lost at power-off";
    try {
        await link.flash("save", SAVE_FULL);
        return null;
    } catch (e) {
        return e.message;
    }
}

export function saveToFlash(link) { return link.flash("save", SAVE_FULL); }

let freeCrc = 0;                                         // of a bank of 0xFF: a free kit bank
const isKitBank = b => b >= 8 && (b <= 26 || b >= 32);

async function deviceCrcs(link, inf) {                   // the CRC-32 of every ROM bank as the Game Boy sees them
    if (!link.crcs || link.crcs.length !== inf.banks) link.crcs = await bankCrcs(link, inf.banks);
    return link.crcs;
}

/* the ROM bank each of a song file's kits has on this FM-1; a kit it lacks goes into a free flash kit bank, or one
   more bank (then the save goes to the computer first: opts.backup(bytes)). Nothing is written unless all fit. */
async function placeKits(link, inf, kits, opts, progress) {
    const crcs = await deviceCrcs(link, inf), want = kits.map(k => crc32(k));
    const banks = want.map(c => crcs.findIndex((v, b) => v === c && isKitBank(b)));
    const missing = banks.map((b, i) => (b < 0 ? i : -1)).filter(i => i >= 0);
    if (!missing.length) return banks;
    const count = `${missing.length} kit${missing.length > 1 ? "s" : ""} that this FM-1 does not have`;
    if (!inf.flash || !inf.nkit) throw new Error(`this song brings ${count}, and it has no kit banks in flash for them`);
    if (!freeCrc) freeCrc = crc32(new Uint8Array(BANK).fill(0xFF));
    const shownAs = [], free = [];
    for (let j = 0; j < inf.kits; j++) {
        shownAs.push(await link.kitBankNo(j));
        if (crcs[shownAs[j]] === freeCrc) free.push(j);
    }
    const more = missing.length - free.length;
    if (more > 0) {
        if (inf.kits + more > inf.nkit)
            throw new Error(`this song brings ${count}, and there is no room for another kit bank (${inf.kits} of ${inf.nkit} are in use): remove a kit first`);
        if (!opts.backup)
            throw new Error(`this song brings ${count}; that takes ${more} more kit bank${more > 1 ? "s" : ""} in flash, and the save must go to the computer first`);
        await opts.backup(await link.read(0, SAV_SIZE, progress));
        await link.flash("kits " + hx(inf.kits + more), "there is no room for another kit bank: the save would no longer fit (delete songs first)");
        for (let j = inf.kits; j < inf.kits + more; j++) {
            shownAs.push(await link.kitBankNo(j));
            crcs[shownAs[j]] = freeCrc;
            free.push(j);
        }
    }
    for (const [n, i] of missing.entries()) {
        await link.writeKit(free[n], kits[i], progress);
        banks[i] = shownAs[free[n]];
        crcs[banks[i]] = want[i];
    }
    return banks;
}

/* a song file onto the FM-1 -> { slot, unsaved }: unsaved is null, or why the change is not in flash.
   opts.backup(bytes): called with the whole save before the number of kit banks changes. */
export function uploadSong(link, bytes, progress = () => {}, opts = {}) {
    const file = parseSongFile(bytes);                   // a damaged file stops here: nothing is sent
    return whileHeld(link, true, async () => {
        const inf = await link.info(), old = await fsHeader(link);
        importSong(old, { ...file, kits: [] });          // no slot, no room, a damaged song: before any kit is stored
        const kitBanks = file.kits.length ? await placeKits(link, inf, file.kits, opts, progress) : [];
        const { sav, slot } = importSong(old, file, kitBanks);
        await writeChange(link, old, sav, progress);
        return { slot, unsaved: await persist(link, inf) };
    });
}

export function deleteSong(link, slot, shown = null) {
    return whileHeld(link, true, async () => {
        const old = await fsHeader(link);
        checkShown(old, slot, shown);
        await writeChange(link, old, removeSong(old, slot));
        return { unsaved: await persist(link, await link.info()) };
    });
}

export function downloadSav(link, progress = () => {}) {
    return whileHeld(link, false, () => link.read(0, SAV_SIZE, progress));
}

/* the whole save. It cannot be replaced in one step: the names and the block table (0x8000..0x81FF) go last, so
   until then LSDj's song list is the old one; a restore that stops leaves a mixed save, and says so */
export function uploadSav(link, bytes, progress = () => {}) {
    if (bytes.length !== SAV_SIZE) throw new Error("not an LSDj save: it must be 131072 bytes");
    if (bytes[0x813E] !== 0x6A || bytes[0x813F] !== 0x6B) throw new Error("not an LSDj save: no file system mark");
    return whileHeld(link, true, async () => {
        const old = await link.read(0, SAV_SIZE, (a, n) => progress(a, 2 * n));
        const pieces = [];
        for (const [a, e] of [[0, FS_HEAD], [BLOCKS, SAV_SIZE], [FS_HEAD, BLOCKS]])
            for (let o = a; o < e; o += WR) {
                const n = Math.min(WR, e - o);
                if (!samePiece(old, bytes, o, n)) pieces.push([o, n]);
            }
        let i = 0;
        for (const [o, n] of pieces) {
            try {
                await link.write(o, bytes.subarray(o, o + n));
            } catch (e) {
                if (!i) throw e;                         // nothing was written yet
                throw new Error(`the save on the FM-1 is incomplete: restore it again (${e.message})`);
            }
            progress(SAV_SIZE + Math.round(SAV_SIZE * ++i / pieces.length), 2 * SAV_SIZE);
        }
        link.crcs = null;
        return { unsaved: await persist(link, await link.info()) };
    });
}

/* the flash kit banks: [{ j, bank (the ROM bank it shows as), name, free }] */
export async function listKits(link) {
    const inf = await link.info(), out = [];
    for (let j = 0; j < inf.kits; j++) {
        const bank = await link.kitBankNo(j), head = await link.data(`b ${hx(bank)} 0 2`, 2);
        const free = head[0] === 0xFF && head[1] === 0xFF;
        let name = "";
        if (!free) for (const c of await link.data(`b ${hx(bank)} 52 6`, 6)) name += c >= 0x20 && c < 0x7F ? String.fromCharCode(c) : " ";
        out.push({ j, bank, name: name.trim(), free });
    }
    return out;
}

export function removeKit(link, j) {                     // (songs that use it lose its sounds)
    return whileHeld(link, true, async () => {
        await link.flash("kx " + hx(j));
        link.crcs = null;
    });
}

/* k kit banks from now on. The save moves in flash, which is not safe against a power cut: it goes to the computer
   first (backup(bytes)). */
export function setKitBanks(link, k, backup, progress = () => {}) {
    return whileHeld(link, true, async () => {
        await backup(await link.read(0, SAV_SIZE, progress));
        await link.flash("kits " + hx(k), "the save would not fit with that many kit banks (delete songs first)");
        link.crcs = null;
    });
}

export async function bankCrcs(link, n) {
    const out = [];
    for (let b = 0; b < n; b++) out.push(await link.bankCrc(b));
    return out;
}

/* Web Serial: open the port and reach the console -> { link, info, close }. onLost: the FM-1 went away (not called
   for our own close). A failed connect leaves the port closed: Web Serial refuses to close a port while a reader or
   writer holds it, so they are let go first. */
export async function openPort(port, onLost = () => {}, timeoutMs = 2000) {
    await port.open({ baudRate: 115200 });
    let reader = null, writer = null, closing = false;
    const close = async () => {
        if (closing) return;
        closing = true;
        for (const step of [() => reader && reader.cancel(), () => reader && reader.releaseLock(),
                            () => writer && writer.releaseLock(), () => port.close()])
            try { await step(); } catch (e) { /* already gone */ }
    };
    try {
        await port.setSignals({ dataTerminalReady: true });   // the console answers only with DTR on
        writer = port.writable.getWriter();
        reader = port.readable.getReader();
        const link = new Link(b => writer.write(b), timeoutMs);
        (async () => {
            for (;;) {
                const { value, done } = await reader.read();
                if (done) break;
                link.feed(value);
            }
        })().catch(() => {}).finally(async () => {
            if (closing) return;
            await close();
            onLost();
        });
        await link.sync();
        return { link, info: await link.info(), close };
    } catch (e) {
        await close();
        throw e;
    }
}
