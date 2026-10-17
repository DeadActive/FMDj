#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""DMGo! on the LSDj unit's emulator (tests/lsdj/lsdj_host.c, the firmware's own code): it runs, its 32 KiB save is
written and read back (Peanut-GB's MBC5 + rumble + RAM). Needs the owner's own DMGo ROM (DMGO_ROM, v1); skipped
without it. Nothing of DMGo is in this repo: the addresses below are where v1 keeps its state (measured on the host,
2026-10-17; firmware/src/lsdj/lsdj_gb.c)."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[2]
rom = os.environ.get("DMGO_ROM")
if not rom:
    print("dmgo_test: skipped (DMGO_ROM)")
    sys.exit(0)
fails = 0


def check(name, cond, extra=""):
    global fails
    print(("ok      " if cond else "FAILED  ") + name + (" " + extra if extra and not cond else ""))
    fails += not cond


host = SRC / "build" / "tests" / "lsdj_host"
host.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(["cc", "-O2", "-w", f"-I{SRC / 'firmware/src'}", f"-I{SRC / 'firmware/src/lsdj'}", "-o", host,
                SRC / "tests" / "lsdj" / "lsdj_host.c"], check=True)
tmp = Path(tempfile.mkdtemp(prefix="dmgo_test_"))
START = ["300:304:t"]                         # past the title screen (PRESS ANY BUTTON)


def presses(keys, at=400, gap=12):
    """-k arguments: each of keys (letters of l r u d a b s t) down 4 frames, one every `gap` frames from `at`;
    -> (the list, the frame after the last)"""
    out = []
    for k in keys:
        out.append(f"{at}:{at + 4}:{k}")
        at += gap
    return out, at


def run(name, frames, ks=(), save=None, extra=()):
    """the host for `frames` frames with these -k ranges -> (stdout, rc, wram bytes, save bytes)"""
    args = [host, rom, str(frames), "-W", tmp / f"{name}.wram", "-o", tmp / f"{name}.sav", *extra]
    for k in ks:
        args += ["-k", k]
    if save:
        args += ["-s", save]
    p = subprocess.run(args, capture_output=True, text=True)
    w = (tmp / f"{name}.wram").read_bytes() if p.returncode in (0, 1) and (tmp / f"{name}.wram").exists() else b""
    s = (tmp / f"{name}.sav").read_bytes() if (tmp / f"{name}.sav").exists() else b""
    return p.stdout + p.stderr, p.returncode, w, s


def at(w, addr):
    return w[addr - 0xC000]


def peak(out):
    m = re.search(r"audio peak (\d+)", out)
    return int(m.group(1)) if m else -1


# 1. it runs: no emulator errors, a picture, its own save size
out, rc, w, s = run("boot", 600, START)
check("1a DMGo runs (no emulator errors, a picture)", rc == 0 and "emulator errors 0" in out, out)
check("1b its save is 32 KiB", len(s) == 32 * 1024, str(len(s)))
check("1c on a channel page after START (C005 = 0, past the title: C02F = 1)",
      w and at(w, 0xC005) == 0 and at(w, 0xC02F) == 1)

# 2. the save RAM: step 3 of CH1 set (cursor to SEQ: 10 x DOWN, 2 x RIGHT, B), then the SAV page (A, 5 x RIGHT, A) and
#    B there saves slot 00; read back by DMGo in a second run (SAV page, RIGHT to LOAD, B), step 3 is set again
ks, end = presses(["d"] * 10 + ["r", "r", "b"] + ["a"] + ["r"] * 5 + ["a", "b"])
out, rc, w, s1 = run("save", end + 60, START + ks)
check("2a step 3 set (C04D bit 2)", w and at(w, 0xC04D) == 0x04)
check("2b DMGo wrote its save RAM", any(s1), "all zero")
ks, end = presses(["a"] + ["r"] * 5 + ["a", "r", "b", "a", "l", "l", "l", "l", "l", "a"])
out, rc, w, s2 = run("load", end + 60, START + ks, save=tmp / "save.sav")
check("2c the save loads back: step 3 set in a fresh run", rc == 0 and w and at(w, 0xC04D) == 0x04,
      f"C04D={at(w, 0xC04D) if w else None}")

print("dmgo_test: " + ("all passed" if not fails else f"{fails} FAILED"))
sys.exit(1 if fails else 0)
