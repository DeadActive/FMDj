// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// With the owner's ROM only (never stored in this repo): the real LSDj, on the firmware's own emulator
// (tests/lsdj/lsdj_host.c), loads a song that web/lsdjfs.js put into a save. Run from the repo root:
//   LSDJ_ROM=your.gb node tests/lsdj/lsdj_song_test.mjs
// 1. LSDj saves the demo song to slot 0 itself (keys on its file screen)
// 2. lsdjfs exports that slot as a song file and adds it twice to the empty start-up save (slots 0 and 1, so
//    slot 1 lies in other blocks and its jumps are rewritten)
// 3. LSDj loads slot 1 of that save; its working memory must equal what LSDj's own load of its own slot gives,
//    outside the timer bytes 0x3FB0..0x3FCF; and the song plays
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { BLOCKS, buildSongFile, importSong, parseSongFile, slotBlocks, songKitBanks, songs, unpackSong } from "../../web/lsdjfs.js";

const ROM = process.env.LSDJ_ROM, HOST = "build/host/lsdj_host", BOOT = "build/lsdj/boot.sav", DEMO = "build/lsdj/demo.sav";
if (!ROM || ![HOST, BOOT, DEMO].every(existsSync)) {
    console.log("lsdj_song_test: skipped (LSDJ_ROM, build/host/lsdj_host and build/lsdj/*.sav: build with FELUCCA_LSDJ=1 first)");
    process.exit(0);
}
let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(72)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };
mkdirSync("build/tests", { recursive: true });
const T = "build/tests/song_";

/* keys for the harness: a letter is pressed for 4 frames, 10 frames apart; "s+u": s held, u pressed inside it;
   a number waits that many frames. Letters: l r u d a b s(elect) t(start). */
function run(save, out, steps, tail = 60) {
    const args = [];
    let f = 60;
    for (const k of steps) {
        if (typeof k === "number") f += k;
        else if (k.includes("+")) {
            const [m, x] = k.split("+");
            args.push("-k", `${f}:${f + 8}:${m}`, "-k", `${f + 2}:${f + 6}:${x}`);
            f += 14;
        } else {
            args.push("-k", `${f}:${f + 4}:${k}`);
            f += 10;
        }
    }
    const text = execFileSync(HOST, [ROM, String(f + tail), "-s", save, "-o", out, ...args], { encoding: "utf8" });
    return { text, sav: new Uint8Array(readFileSync(out)) };
}
const FILE_SCREEN = ["s+u", ...Array(10).fill("d"), "a", 60];        // PROJECT, down to LOAD/SAVE SONG, A

/* 1. LSDj's own save of the demo song: RIGHT (SAVE), A (the slot list), A (SAVE AS), A (OK) */
const ref = run(DEMO, T + "ref.sav", [...FILE_SCREEN, "r", "a", 60, "a", 60, "a", 900]).sav;
const list = songs(ref);
ok(list.length === 1 && list[0].slot === 0 && list[0].blocks >= 1 && ref[0x8140] === 0, "LSDj saved the demo song to slot 0");
const song = unpackSong(ref, 0);
ok(song !== null, "lsdjfs unpacks LSDj's own slot to a whole song");

/* 2. out as a file, into the empty save twice */
ok(songKitBanks(ref, 0).length === 0, "the demo song uses no kits");
const file = parseSongFile(buildSongFile(ref, 0, []));
const boot = new Uint8Array(readFileSync(BOOT));
ok(songs(boot).length === 0, "the start-up save has no songs");
const two = importSong(importSong(boot, file).sav, file);
ok(two.slot === 1 && slotBlocks(two.sav, 1)[0] === list[0].blocks, "the second copy lies in the blocks after the first");
ok(unpackSong(two.sav, 1).every((v, i) => v === song[i]), "it unpacks to the same song there");
writeFileSync(T + "put.sav", two.sav);

/* 3. LSDj loads it: A (the slot list, LOAD), DOWN to slot 1, A */
const own = run(T + "ref.sav", T + "own.sav", [...FILE_SCREEN, "a", 60, "a", 600]).sav;
const got = run(T + "put.sav", T + "got.sav", [...FILE_SCREEN, "a", 60, "d", "a", 600]).sav;
ok(got[0x8140] === 1, "LSDj made slot 1 its active song");
const diff = [];
for (let i = 0; i < 0x8000; i++) if (got[i] !== own[i] && (i < 0x3FB0 || i > 0x3FCF)) diff.push(i);
if (diff.length) console.log("   differing addresses: " + diff.slice(0, 16).map(a => a.toString(16)).join(" "));
ok(diff.length === 0, "LSDj's working memory equals its own load of its own save (outside the timers)");
let before = 0;
for (let i = 0; i < 0x8000; i++) if (boot[i] !== got[i]) before++;
ok(before > 100, "... and it is not the empty song it started from");
ok(got.subarray(BLOCKS).every((v, i) => v === two.sav[BLOCKS + i]), "LSDj left the blocks as they were written");
const play = execFileSync(HOST, [ROM, "600", "-s", T + "got.sav", "-k", "60:64:t"], { encoding: "utf8" });
const peak = /audio peak (\d+)/.exec(play);
ok(peak && +peak[1] >= 2000, "START plays the loaded song");

console.log(failed ? `lsdj_song_test: ${failed} FAILED` : "lsdj_song_test: all passed");
process.exit(failed ? 1 : 0);
