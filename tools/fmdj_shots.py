#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""FMDj landing page screenshots: walks the owner's own LSDj ROM to each screen on the host emulator
(tests/lsdj/lsdj_host.c) and writes one PNG per screen and a manifest.

  fmdj_shots.py ROM SAVE OUTDIR

A save in an older LSDj song format makes LSDj 9.4.2 show "UPGRADING..." while it converts the song, so the first
~220 frames of a run from power-on (song, chain, phrase, groove, project) would all show that message. A settle
pass therefore runs the ROM with SAVE and no keys for SETTLE frames, writes the converted save RAM to a temporary
file in OUTDIR (deleted at the end), and every screen's run starts from that settled save.

LSDj's licence forbids distributing it: the screenshots are made from the ROM, so OUTDIR must be git-ignored
(checked), and nothing written here is ever committed or published.
web/make_site.py takes the screenshots from build/fmdj/shots (or $FMDJ_SHOTS), so make them with
tools/fmdj_shots.py $LSDJ_ROM $FMDJ_SAVE build/fmdj/shots"""
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


def keys_for(path, start=120):
    """The host's -k arguments: START at 100..103 (starts the song), then every 20 frames from START: SELECT
    held 5 frames, the direction in its third frame (LSDj needs SELECT held before the direction)."""
    args = ["-k", "100:104:t"]
    for i, d in enumerate(path):
        f = start + 20 * i
        args += ["-k", f"{f}:{f + 5}:s", "-k", f"{f + 2}:{f + 3}:{d}"]
    return args


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


def main(rom, sav, out):
    out = Path(out).resolve()
    if subprocess.run(["git", "-C", SRC, "check-ignore", "-q", out], capture_output=True).returncode:
        raise SystemExit(f"fmdj_shots: {out} is not git-ignored: screenshots of the ROM must never be committed")
    for what, f in (("ROM", rom), ("SAVE", sav)):
        if not Path(f).is_file():
            raise SystemExit(f"fmdj_shots: the {what} {f} is not an existing file (nothing written)")
    top = foreign_repo(out)
    if top:
        raise SystemExit(f"fmdj_shots: {out} is inside another git repository ({top}): screenshots of the ROM "
                         f"must not be put where they could be committed or published")
    host = SRC / "build" / "host" / "lsdj_host"
    host.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-w", f"-I{SRC / 'firmware/src'}", f"-I{SRC / 'firmware/src/lsdj'}", "-o", host,
                    SRC / "tests" / "lsdj" / "lsdj_host.c"], check=True)
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
            manifest.append({"name": name, "title": name.upper(), "file": name + ".png"})
            print(f"fmdj_shots: {name:8} {frames} frames -> {out / (name + '.png')}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*sys.argv[1:4])
