#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""tools/fmdj_shots.py: the walk to each LSDj screen and each DMGo screen, the PNGs, the output refusal. Part 5 needs
the owner's ROM (LSDJ_ROM) and build/lsdj/demo.sav, part 8 the owner's DMGo ROM (DMGO_ROM); skipped without them."""
import json
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SRC / "tools"))
fails = 0


def check(name, cond, extra=""):
    global fails
    print(("ok      " if cond else "FAILED  ") + name + (" " + extra if extra and not cond else ""))
    fails += not cond


def decode(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        assert zlib.crc32(typ + body) == struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0]
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    return ihdr, zlib.decompress(idat)


try:
    import fmdj_shots as F
except ImportError as e:
    print("FAILED  import tools/fmdj_shots.py:", e)
    print("1 FAILED")
    sys.exit(1)

want = {"song": [], "chain": ["r"], "phrase": ["r", "r"], "instr": ["r", "r", "r"], "table": ["r", "r", "r", "r"],
        "synth": ["r", "r", "u"], "wave": ["r", "r", "r", "r", "u"], "groove": ["d"], "project": ["u"]}
check("1 PATHS", F.PATHS == want and list(F.PATHS) == list(want))
check("2 keys_for", F.keys_for(["r", "u"], start=120) ==
      ["-k", "100:104:t", "-k", "120:125:s", "-k", "122:123:r", "-k", "140:145:s", "-k", "142:143:u"])
pal = [(0x9B, 0xBC, 0x0F), (0x8B, 0xAC, 0x0F), (0x30, 0x62, 0x30), (0x0F, 0x38, 0x0F)]
check("3a PALETTE", list(F.PALETTE) == pal)
shades = [(x // 40) % 4 for y in range(144) for x in range(160)]
ihdr, raw = decode(F.png(shades))
check("3b IHDR", ihdr[:5] == (480, 432, 8, 2, 0), str(ihdr))
rows = [raw[i * (480 * 3 + 1):(i + 1) * (480 * 3 + 1)] for i in range(432)]
ok = len(raw) == 432 * (480 * 3 + 1) and all(r[0] == 0 for r in rows)
ok = ok and all(tuple(rows[y][1 + x * 3:4 + x * 3]) == pal[(x // 3 // 40) % 4] for y in (0, 100, 431) for x in range(480))
check("3c pixels", ok)

rom = os.environ.get("LSDJ_ROM")
sav = SRC / "build" / "lsdj" / "demo.sav"
tool = SRC / "tools" / "fmdj_shots.py"
if rom and sav.exists():
    bad = Path("/tmp/x_fmdj_shots")
    r = subprocess.run([sys.executable, tool, rom, sav, bad], capture_output=True, text=True)
    check("4 refuses a path that is not git-ignored", r.returncode != 0 and "git-ignored" in r.stdout + r.stderr
          and not bad.exists())
    out = SRC / "build" / "tests" / "fmdj_shots"
    r = subprocess.run([sys.executable, tool, rom, sav, out], capture_output=True, text=True)
    check("5a tool runs", r.returncode == 0, r.stdout + r.stderr)
    names = list(want)
    man = json.loads((out / "manifest.json").read_text()) if (out / "manifest.json").exists() else []
    check("5b manifest", man == [{"prog": "lsdj", "name": n, "title": n.upper(), "file": n + ".png"} for n in names])
    files = [(out / (n + ".png")).read_bytes() if (out / (n + ".png")).exists() else None for n in names]
    check("5c nine PNGs", all(files) and len(set(files)) == 9)
else:
    r = subprocess.run([sys.executable, tool, "rom", "sav", "/tmp/x_fmdj_shots"], capture_output=True, text=True)
    check("4 refuses a path that is not git-ignored", r.returncode != 0 and "git-ignored" in r.stdout + r.stderr
          and not Path("/tmp/x_fmdj_shots").exists())
    print("skipped 5 (LSDJ_ROM or build/lsdj/demo.sav missing)")

# ---- 5d an older-format save: LSDj shows UPGRADING... for a while, which the settle pass must outwait
fsav = os.environ.get("FMDJ_SAVE")
if rom and fsav and Path(fsav).is_file():
    out = SRC / "build" / "tests" / "fmdj_shots_dontstop"
    subprocess.run(["rm", "-rf", str(out)])
    r = subprocess.run([sys.executable, tool, rom, fsav, out], capture_output=True, text=True)
    check("5d tool runs with FMDJ_SAVE", r.returncode == 0, r.stdout + r.stderr)
    files = [(out / (n + ".png")).read_bytes() if (out / (n + ".png")).exists() else None for n in want]
    check("5e nine different PNGs (no UPGRADING... twins)", all(files) and len(set(files)) == 9,
          f"{len(set(files))} distinct")
    left = sorted(p.name for p in out.iterdir() if p.suffix not in (".png", ".json"))
    check("5f no settled-save temp file left", not left, str(left))
else:
    print("skipped 5d-f (LSDJ_ROM or FMDJ_SAVE missing)")

# ---- 6 missing inputs / foreign repository (no ROM needed)
tdir = SRC / "build" / "tests" / "fmdj_shots_refuse"
subprocess.run(["rm", "-rf", str(tdir)])
tdir.mkdir(parents=True)
fake = tdir / "fake.gb"
fake.write_bytes(b"x")
o1 = tdir / "out1"
r = subprocess.run([sys.executable, tool, fake, tdir / "nosuch.sav", o1], capture_output=True, text=True)
check("6a refuses a missing SAVE, naming it", r.returncode != 0 and "nosuch.sav" in r.stdout + r.stderr
      and not o1.exists(), r.stdout + r.stderr)
(tdir / "some.sav").write_bytes(b"x")
r = subprocess.run([sys.executable, tool, tdir / "nosuch.gb", tdir / "some.sav", o1], capture_output=True, text=True)
check("6b refuses a missing ROM, naming it", r.returncode != 0 and "nosuch.gb" in r.stdout + r.stderr
      and not o1.exists(), r.stdout + r.stderr)
other = tdir / "otherrepo"
other.mkdir()
subprocess.run(["git", "init", "-q", str(other)], check=True)
o2 = other / "deep" / "shots"
r = subprocess.run([sys.executable, tool, fake, tdir / "some.sav", o2], capture_output=True, text=True)
check("6c refuses an output inside another git repository", r.returncode != 0 and "repository" in r.stdout + r.stderr
      and str(other.resolve()) in r.stdout + r.stderr and not o2.exists(), r.stdout + r.stderr)

# ---- 7 DMGo's walk (no ROM needed): the screens, the page each one ends on (WRAM C005), the host's arguments
dwant = ["title", "ch1", "ch3", "ch4", "pick", "patterns", "save", "setup"]
check("7a DMGO screens", list(F.DMGO) == dwant, list(getattr(F, "DMGO", {})))
pages = {n: F.DMGO[n]["page"] for n in F.DMGO}
check("7b DMGO pages (C005: CH1..CH4 0..3, PATTERNS 4, SAVE 5, SETUP 6; the title none)",
      pages == {"title": None, "ch1": 0, "ch3": 2, "ch4": 3, "pick": 0, "patterns": 4, "save": 5, "setup": 6}, pages)
titles = [F.DMGO[n]["title"] for n in dwant]
check("7c DMGO titles distinct and upper case", len(set(titles)) == len(titles)
      and all(t == t.upper() for t in titles), titles)
args, frames = F.dmgo_args(["a", "r"], [1, 5])
check("7d dmgo_args: START at 300, white keys from 400 every 16 frames, then keys every 12",
      args == ["-k", "300:304:t", "-n", "400:1", "-n", "416:5", "-k", "432:436:a", "-k", "444:448:r"]
      and frames == 456 + 60, (args, frames))
args, frames = F.dmgo_args([], [], start=False, frames=250)
check("7e dmgo_args: the title screen (no START)", args == [] and frames == 250, (args, frames))
check("7f CH1 is filled with the white keys, the others with DMGo's own presets (PICK A PAT: SELECT, RIGHT, B)",
      F.DMGO["ch1"]["steps"] and not F.DMGO["ch3"]["steps"] and F.DMGO["ch3"]["keys"][4:7] == ["s", "r", "b"])
o3 = tdir / "out3"
r = subprocess.run([sys.executable, tool, "--dmgo", tdir / "nosuch.gb", o3], capture_output=True, text=True)
check("7g --dmgo refuses a missing ROM, naming it", r.returncode != 0 and "nosuch.gb" in r.stdout + r.stderr
      and not o3.exists(), r.stdout + r.stderr)
r = subprocess.run([sys.executable, tool, "--dmgo", fake, "/tmp/x_fmdj_dmgo"], capture_output=True, text=True)
check("7h --dmgo refuses a path that is not git-ignored", r.returncode != 0 and "git-ignored" in r.stdout + r.stderr
      and not Path("/tmp/x_fmdj_dmgo").exists(), r.stdout + r.stderr)
o4 = other / "deep" / "dmgo"
r = subprocess.run([sys.executable, tool, "--dmgo", fake, o4], capture_output=True, text=True)
check("7i --dmgo refuses an output inside another git repository", r.returncode != 0
      and str(other.resolve()) in r.stdout + r.stderr and not o4.exists(), r.stdout + r.stderr)

# ---- 8 DMGo's screens from the owner's ROM
drom = os.environ.get("DMGO_ROM")
if drom and Path(drom).is_file():
    out = SRC / "build" / "tests" / "fmdj_shots_dmgo"
    subprocess.run(["rm", "-rf", str(out)])
    r = subprocess.run([sys.executable, tool, "--dmgo", drom, out], capture_output=True, text=True)
    check("8a --dmgo runs", r.returncode == 0, r.stdout + r.stderr)
    man = json.loads((out / "manifest.json").read_text()) if (out / "manifest.json").exists() else []
    check("8b manifest: prog dmgo, the screens in order",
          man == [{"prog": "dmgo", "name": n, "title": F.DMGO[n]["title"], "file": n + ".png"} for n in dwant], man)
    files = [(out / (n + ".png")).read_bytes() if (out / (n + ".png")).exists() else None for n in dwant]
    check("8c eight different PNGs", all(files) and len(set(files)) == len(dwant), f"{len(set(f for f in files if f))}")
    def greens(f):
        ihdr, raw = decode(f)
        n = 480 * 3 + 1
        return ihdr[:2] == (480, 432) and len(raw) == 432 * n and {
            raw[y * n + 1 + x * 3:y * n + 4 + x * 3] for y in range(0, 432, 3) for x in range(0, 480, 3)} <= \
            {bytes(c) for c in pal}
    check("8d each 480 x 432 in the four greens", all(f and greens(f) for f in files))
    left = sorted(p.name for p in out.iterdir() if p.suffix not in (".png", ".json"))
    check("8e nothing else left in the output", not left, str(left))
    if rom:
        o5 = SRC / "build" / "tests" / "fmdj_shots_notdmgo"
        subprocess.run(["rm", "-rf", str(o5)])
        r = subprocess.run([sys.executable, tool, "--dmgo", rom, o5], capture_output=True, text=True)
        check("8f --dmgo with LSDj's ROM fails: a screen not where DMGo's walk ends", r.returncode != 0
              and "page" in r.stdout + r.stderr and not (o5 / "manifest.json").exists(), r.stdout + r.stderr)
else:
    print("skipped 8 (DMGO_ROM missing)")

print("fmdj_shots_test: all passed" if not fails else f"{fails} FAILED")
sys.exit(fails != 0)
