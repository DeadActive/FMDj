#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""FMDj landing page screenshots: walks the owner's own LSDj or DMGo! ROM to each screen on the host emulator
(tests/lsdj/lsdj_host.c) and writes one PNG per screen (160 x 144 at 3x in the Game Boy's greens) and a manifest
(manifest.json: prog, name, title, file per screen).

  fmdj_shots.py ROM SAVE OUTDIR       LSDj's screens, from a song in SAVE
  fmdj_shots.py --dmgo ROM OUTDIR     DMGo's screens (DMGo v1), from a pattern made in the run

A save in an older LSDj song format makes LSDj 9.4.2 show "UPGRADING..." while it converts the song, so the first
~220 frames of a run from power-on (song, chain, phrase, groove, project) would all show that message. A settle
pass therefore runs the ROM with SAVE and no keys for SETTLE frames, writes the converted save RAM to a temporary
file in OUTDIR (deleted at the end), and every screen's run starts from that settled save.

DMGo starts empty: CH1's steps are set with the FM-1's white keys (-n, as the firmware presses them), CH3's and CH4's
with DMGo's own presets (PICK A PAT), and START plays them. Each run ends on a known page (WRAM C005, measured on
DMGo v1, 2026-10-17); a ROM that ends elsewhere is refused.

Neither program may be distributed: the screenshots are made from the ROM, so OUTDIR must be git-ignored and not
inside another git repository (checked), and nothing written here is ever committed or published.
web/make_site.py takes LSDj's from build/fmdj/shots (or $FMDJ_SHOTS) and DMGo's from build/fmdj/dmgo-shots (or
$FMDJ_DMGO_SHOTS), so make them with
  tools/fmdj_shots.py $LSDJ_ROM $FMDJ_SAVE build/fmdj/shots
  tools/fmdj_shots.py --dmgo $DMGO_ROM build/fmdj/dmgo-shots"""
import json
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
PALETTE = [(0x9B, 0xBC, 0x0F), (0x8B, 0xAC, 0x0F), (0x30, 0x62, 0x30), (0x0F, 0x38, 0x0F)]   # light to dark
W, H = 160, 144
SETTLE = 900   # frames (15 s of Game Boy time), well past the older-format conversion
PATHS = {"song": [], "chain": ["r"], "phrase": ["r", "r"], "instr": ["r", "r", "r"],
         "table": ["r", "r", "r", "r"], "synth": ["r", "r", "u"], "wave": ["r", "r", "r", "r", "u"],
         "groove": ["d"], "project": ["u"]}
# DMGo's screens: keys (letters of the host's -k: a b s t u d l r), white keys (steps 1..16), the page it ends on
# (WRAM C005: 0..3 CH1..CH4, 4 PATTERNS, 5 SAVE, 6 SETUP; None: the title, before START). The top menu: A opens it
# (CH1 CH2 CH3 CH4 PAT SAV SETUP), RIGHT moves along it, A takes the page. SELECT on a channel page opens PICK A PAT,
# RIGHT picks, B stamps it into the SEQ row, SELECT closes it.
PICK = ["s", "r", "b", "s"]
DMGO = {
    "title": {"title": "TITLE", "keys": [], "steps": [], "page": None},
    "ch1": {"title": "CH1 PULSE", "keys": ["t"], "steps": [1, 4, 7, 9, 11, 13, 15], "page": 0},
    "ch3": {"title": "CH3 WAVE", "keys": ["a", "r", "r", "a"] + PICK + ["t"], "steps": [], "page": 2},
    "ch4": {"title": "CH4 NOISE", "keys": ["a", "r", "r", "r", "a", "s", "r", "r", "b", "s", "t"], "steps": [],
            "page": 3},
    "pick": {"title": "PICK A PAT", "keys": ["s", "r", "b"], "steps": [], "page": 0},
    "patterns": {"title": "PATTERNS", "keys": PICK + ["t", "a"] + ["r"] * 4 + ["a"], "steps": [], "page": 4},
    "save": {"title": "SAVE", "keys": ["a"] + ["r"] * 5 + ["a"], "steps": [], "page": 5},
    "setup": {"title": "SETUP", "keys": ["a"] + ["r"] * 6 + ["a"], "steps": [], "page": 6},
}
DMGO_TITLE_FRAMES = 250   # the title screen drawn, no key pressed


def keys_for(path, start=120):
    """The host's -k arguments: START at 100..103 (starts the song), then every 20 frames from START: SELECT
    held 5 frames, the direction in its third frame (LSDj needs SELECT held before the direction)."""
    args = ["-k", "100:104:t"]
    for i, d in enumerate(path):
        f = start + 20 * i
        args += ["-k", f"{f}:{f + 5}:s", "-k", f"{f + 2}:{f + 3}:{d}"]
    return args


def dmgo_args(keys, steps, start=True, frames=None):
    """The host's arguments for a DMGo walk -> (args, frames to run): START at 300..303 (past PRESS ANY BUTTON), the
    white keys from frame 400 one every 16 frames, then the keys one every 12 frames (each down 4 frames); the
    picture 60 frames after the last. Without START (the title screen): nothing pressed, FRAMES frames."""
    if not start:
        return [], frames
    args, at = ["-k", "300:304:t"], 400
    for n in steps:
        args += ["-n", f"{at}:{n}"]
        at += 16
    for k in keys:
        args += ["-k", f"{at}:{at + 4}:{k}"]
        at += 12
    return args, at + 60


def _chunk(typ, body):
    return struct.pack(">I", len(body)) + typ + body + struct.pack(">I", zlib.crc32(typ + body))


def png(shades, scale=3):
    """RGB PNG of W*H shades (0..3), scaled by nearest neighbour."""
    rows = []
    for y in range(H):
        line = b"".join(bytes(PALETTE[shades[y * W + x]]) * scale for x in range(W))
        rows += [b"\0" + line] * scale
    ihdr = struct.pack(">IIBBBBB", W * scale, H * scale, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
            + _chunk(b"IEND", b""))


def read_pgm(path):
    d = Path(path).read_bytes()
    head = b"P5 160 144 255\n"
    if not d.startswith(head) or len(d) != len(head) + W * H:
        raise SystemExit(f"fmdj_shots: {path}: not the host's 160x144 PGM")
    return [(255 - v) // 85 for v in d[len(head):]]


def foreign_repo(path):
    """The top level of the git repository other than this one that contains PATH (or its nearest existing
    parent), else None."""
    d = Path(path).resolve()
    while not d.exists():
        d = d.parent
    r = subprocess.run(["git", "-C", d, "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    top = r.stdout.strip()
    if r.returncode or not top or Path(top).resolve() == SRC.resolve():
        return None
    return top


def guard(out, inputs):
    """OUTDIR (resolved) after the checks: git-ignored here, the inputs existing files, not in another repository"""
    out = Path(out).resolve()
    if subprocess.run(["git", "-C", SRC, "check-ignore", "-q", out], capture_output=True).returncode:
        raise SystemExit(f"fmdj_shots: {out} is not git-ignored: screenshots of the ROM must never be committed")
    for what, f in inputs:
        if not Path(f).is_file():
            raise SystemExit(f"fmdj_shots: the {what} {f} is not an existing file (nothing written)")
    top = foreign_repo(out)
    if top:
        raise SystemExit(f"fmdj_shots: {out} is inside another git repository ({top}): screenshots of the ROM "
                         f"must not be put where they could be committed or published")
    return out


def build_host():
    host = SRC / "build" / "host" / "lsdj_host"
    host.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-w", f"-I{SRC / 'firmware/src'}", f"-I{SRC / 'firmware/src/lsdj'}", "-o", host,
                    SRC / "tests" / "lsdj" / "lsdj_host.c"], check=True)
    return host


def main(rom, sav, out):
    out = guard(out, (("ROM", rom), ("SAVE", sav)))
    host = build_host()
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    settled = out / ".settled.sav"
    try:
        r = subprocess.run([host, rom, str(SETTLE), "-s", sav, "-o", settled], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"fmdj_shots: settle pass: the host emulator failed ({r.returncode}): "
                             f"{(r.stdout + r.stderr).strip()}")
        _shots(host, rom, settled, out, manifest)
    finally:
        settled.unlink(missing_ok=True)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")


def main_dmgo(rom, out):
    out = guard(out, (("ROM", rom),))
    host = build_host()
    shots = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name, s in DMGO.items():
            pgm, wram = Path(tmp) / (name + ".pgm"), Path(tmp) / (name + ".wram")
            args, frames = dmgo_args(s["keys"], s["steps"], start=s["page"] is not None, frames=DMGO_TITLE_FRAMES)
            r = subprocess.run([host, rom, str(frames), *args, "-p", pgm, "-W", wram], capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(f"fmdj_shots: {name}: the host emulator failed ({r.returncode}): "
                                 f"{(r.stdout + r.stderr).strip()}")
            w = wram.read_bytes()
            page, past = w[0x05], w[0x2F]               # C005 the page, C02F 1 once past the title
            if (s["page"] is None and past != 0) or (s["page"] is not None and (past != 1 or page != s["page"])):
                raise SystemExit(f"fmdj_shots: {name}: DMGo's page is {page} (past the title: {past}), not "
                                 f"{s['page']}: is {rom} DMGo v1? (nothing written)")
            shots[name] = png(read_pgm(pgm))
            print(f"fmdj_shots: {name:8} {frames} frames, page {page}")
    out.mkdir(parents=True, exist_ok=True)
    for name, data in shots.items():
        (out / (name + ".png")).write_bytes(data)
    (out / "manifest.json").write_text(json.dumps(
        [{"prog": "dmgo", "name": n, "title": DMGO[n]["title"], "file": n + ".png"} for n in DMGO], indent=1) + "\n")
    print(f"fmdj_shots: {len(shots)} DMGo screens -> {out}")


def _shots(host, rom, sav, out, manifest):
    with tempfile.TemporaryDirectory() as tmp:
        for name, path in PATHS.items():
            pgm = Path(tmp) / (name + ".pgm")
            frames = 120 + 20 * len(path) + 60
            r = subprocess.run([host, rom, str(frames), "-s", str(sav), *keys_for(path), "-p", pgm],
                               capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(f"fmdj_shots: {name}: the host emulator failed ({r.returncode}): "
                                 f"{(r.stdout + r.stderr).strip()}")
            (out / (name + ".png")).write_bytes(png(read_pgm(pgm)))
            manifest.append({"prog": "lsdj", "name": name, "title": name.upper(), "file": name + ".png"})
            print(f"fmdj_shots: {name:8} {frames} frames -> {out / (name + '.png')}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--dmgo":
        main_dmgo(*sys.argv[2:4])
    elif len(sys.argv) == 4:
        main(*sys.argv[1:4])
    else:
        raise SystemExit(__doc__)
