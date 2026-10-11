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

    until(found) {                                       // found(text) gives something, or the time is up
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => { this.wake = null; reject(new Error("the FM-1 does not answer")); }, this.timeoutMs);
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
    cmd(line) {
        const run = async () => {
            const echo = `ble ${line}\r\n`;
            this.text = "";
            await this.send(ascii(`ble ${line}\r`));
            const reply = await this.until(t => {
                const i = t.lastIndexOf(echo);
                return i >= 0 && /\r\n> $/.test(t.slice(i + echo.length - 2)) ? t.slice(i + echo.length) : null;
            });
            const m = /^(ok|err)(?: ([^\r\n]*))?\r\n> $/.exec(reply);
            if (!m) throw new Error("this FM-1's firmware has no transfer command: install the LSDj build that has it");
            if (m[1] === "err") throw new Error("FM-1: " + (m[2] || "error"));
            return m[2] || "";
        };
        const p = this.queue.then(run, run);
        this.queue = p.catch(() => {});
        return p;
    }

    async info() {
        const m = /^v([0-9A-F]+) started ([01]) held ([01]) banks ([0-9A-F]+)$/.exec(await this.cmd("info"));
        if (!m) throw new Error("FM-1: an info reply this page does not know");
        return { version: parseInt(m[1], 16), started: m[2] === "1", held: m[3] === "1", banks: parseInt(m[4], 16) };
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

    async write(addr, bytes, progress = () => {}) {      // the save RAM (held); a piece is sent again on a bad CRC
        for (let o = 0; o < bytes.length; o += WR) {
            const d = bytes.subarray(o, o + WR), line = `w ${hx(addr + o)} ${hexBytes(d)} ${hx(crc16(d), 4)}`;
            for (let t = 0; ; t++) {
                try { await this.cmd(line); break; } catch (e) {
                    if (t + 1 === TRIES || !/FM-1: (crc|hex)|does not answer/.test(e.message)) throw e;
                    if (/does not answer/.test(e.message)) await this.sync().catch(() => {});
                }
            }
            progress(Math.min(o + WR, bytes.length), bytes.length);
        }
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

/* bankCrcs: the CRC-32 of each ROM bank of this FM-1 (bankCrcs() below), to find the song's kits there */
export function uploadSong(link, bytes, romCrcs, progress = () => {}) {
    const file = parseSongFile(bytes);                   // a damaged file stops here: nothing is sent
    const kitBanks = file.kits.map(k => romCrcs.indexOf(crc32(k)));
    const missing = kitBanks.filter(b => b < 0).length;
    if (missing)
        throw new Error(`this song brings ${missing} kit${missing > 1 ? "s" : ""} that this FM-1 does not have (custom kits arrive with saving to flash)`);
    return whileHeld(link, true, async () => {
        const old = await fsHeader(link), { sav, slot } = importSong(old, file, kitBanks);
        await writeChange(link, old, sav, progress);
        return slot;
    });
}

export function deleteSong(link, slot, shown = null) {
    return whileHeld(link, true, async () => {
        const old = await fsHeader(link);
        checkShown(old, slot, shown);
        await writeChange(link, old, removeSong(old, slot));
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
