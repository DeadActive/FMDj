#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The LSDj build's speed options change nothing LSDj does: typing the demo song (tools/lsdj_demo.py) and playing
it give the same sound and save RAM (lsdj_host's hash) in every emu_mode. Needs the owner's ROM (LSDJ_ROM) and the
start-up save made from it (build/lsdj/boot.sav, tools/lsdj_rom.py); skipped without them."""
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
hashes = {}
for mode in (0, 1, 2, 3):
    r = subprocess.run([host, rom, str(end + 900), "-s", boot, "-m", str(mode), *keys], capture_output=True, text=True)
    m = re.search(r"emulator errors (\d+).*audio peak (\d+).*hash ([0-9a-f]+), ([\d.]+) us", r.stdout)
    if r.returncode or not m or m.group(1) != "0" or int(m.group(2)) < 2000:
        sys.exit(f"lsdj_modes_test: mode {mode} did not run: {r.stdout.strip()} {r.stderr.strip()}")
    hashes[mode] = m.group(3)
    print(f"mode {mode}: hash {m.group(3)}, {m.group(4)} us/frame on this Mac")
if len(set(hashes.values())) != 1:
    sys.exit("lsdj_modes_test: FAILED: the speed options change the emulation")
print("lsdj_modes_test: all modes identical")
