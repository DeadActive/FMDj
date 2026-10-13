#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The LSDj build's speed options change nothing LSDj does: typing the demo song (tools/lsdj_demo.py) and playing
it give the same sound, machine (registers, counters, memory after every frame) and save RAM (lsdj_host's hash)
with each of them alone and all together (emu_mode 1 inline ROM read, 2 frame skip, 4 the wait loop skipped), and
with both kinds of sound (8: within the frame; the two kinds differ, as they must). And LSDj's wait loop is found
and skipped. (All 16 combinations were run once, 2026-10-13: the same.)
Needs the owner's ROM (LSDJ_ROM) and the start-up save made from it (build/lsdj/boot.sav, tools/lsdj_rom.py);
skipped without them."""
import os
import re
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SRC / "tools"))
import lsdj_demo  # noqa: E402

rom, boot = os.environ.get("LSDJ_ROM"), SRC / "build" / "lsdj" / "boot.sav"
if not rom or not boot.exists():
    print("lsdj_modes_test: skipped (LSDJ_ROM and build/lsdj/boot.sav: build with FELUCCA_LSDJ=1 first)")
    sys.exit(0)
host = SRC / "build" / "tests" / "lsdj_host"
host.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(["cc", "-O2", "-w", f"-I{SRC / 'firmware/src'}", f"-I{SRC / 'firmware/src/lsdj'}", "-o", host,
                SRC / "tests" / "lsdj" / "lsdj_host.c"], check=True)
keys, end = lsdj_demo.key_args(60)
keys += ["-k", f"{end + 30}:{end + 34}:t"]                 # then START: the song plays
MODES = (0, 1, 2, 4, 7, 8, 15)          # each speed option alone and all together; both sounds without and with
runs = {mode: subprocess.Popen([host, rom, str(end + 900), "-s", boot, "-m", str(mode), *keys],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for mode in MODES}
hashes = {}
for mode, p in runs.items():
    out, err = p.communicate()
    m = re.search(r"emulator errors (\d+).*audio peak (\d+).*hash ([0-9a-f]+), ([\d.]+) us.* (\d+) instructions", out)
    if p.returncode or not m or m.group(1) != "0" or int(m.group(2)) < 2000:
        sys.exit(f"lsdj_modes_test: mode {mode} did not run: {out.strip()} {err.strip()}")
    hashes[mode] = m.group(3)
    print(f"mode {mode:2}: hash {m.group(3)}, {m.group(5)} instructions a frame skipped")
    if (int(m.group(5)) > 2000) != bool(mode & 4):
        sys.exit("lsdj_modes_test: FAILED: LSDj's wait loop is skipped only with mode bit 4, and then it must be")
if len({hashes[m] for m in (0, 1, 2, 4, 7)}) != 1 or hashes[8] != hashes[15]:
    sys.exit("lsdj_modes_test: FAILED: the speed options change the emulation")
if hashes[0] == hashes[8]:
    sys.exit("lsdj_modes_test: FAILED: the sound within the frame is the sound per frame")
print("lsdj_modes_test: the speed options change nothing, with either sound")
