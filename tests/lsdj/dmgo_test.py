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

# 3. the step keys (-n: a white key, through lsdj_gb.c's lsdj_steps_* as the firmware runs them)
STEPS = (0xC04D, 0xC04E, 0xC0C1, 0xC0C2, 0xC801, 0xC802, 0xC875, 0xC876)   # CH1: the edited slot, the playing ones
CURSOR = (0xC01A, 0xC01B, 0xCB1F)


def state(w, addrs):
    return [at(w, a) for a in addrs] if w else None


_, _, w0, _ = run("idle", 560, START)
_, _, wk, _ = run("key3", 560, START, extra=["-n", "500:3"])
ks, end = presses(["d"] * 10 + ["r", "r", "b"])
_, _, wb, _ = run("b3", end + 60, START + ks)
check("3a white key 3 = B on step 3 (the same step bytes in all four copies)",
      state(wk, STEPS) == state(wb, STEPS) and at(wk, 0xC04D) == 0x04, f"{state(wk, STEPS)} {state(wb, STEPS)}")
check("3b ... and the cursor stays where it was", state(wk, CURSOR) == state(w0, CURSOR),
      f"{state(wk, CURSOR)} {state(w0, CURSOR)}")
# B cycles a step through DMGo's three states (off, on, the second mask at CA70 / CB00 drawn crossed); so do keys
MASKS = (0xC04D, 0xC0C1, 0xC801, 0xC875, 0xCA70, 0xCB00)
for n in (2, 3):
    _, _, wk, _ = run(f"key3x{n}", 500 + 40 * n, START, extra=[a for i in range(n) for a in ("-n", f"{500 + 40 * i}:3")])
    ks, end = presses(["d"] * 10 + ["r", "r"] + ["b"] * n)
    _, _, wb, _ = run(f"b3x{n}", end + 60, START + ks)
    check(f"3c pressed {n} times = B {n} times", state(wk, MASKS) == state(wb, MASKS),
          f"{state(wk, MASKS)} {state(wb, MASKS)}")
check("3c ... the third press clears it", at(wk, 0xC04D) == 0 and at(wk, 0xCA70) == 0)
# pattern slot 2 being edited, slot 1 playing: on the PAT row (one DOWN) B held and RIGHT
SLOT2 = ["400:404:d", "420:450:b", "428:432:r"]
_, _, wk, _ = run("slot2key", 600, START + SLOT2, extra=["-n", "520:5"])
ks, end = presses(["d"] * 9 + ["r"] * 4 + ["b"], at=470)
_, _, wb, _ = run("slot2b", end + 60, START + SLOT2 + ks)
check("3d in slot 2 (not playing) the same as B: only the edited slot's step",
      state(wk, STEPS) == state(wb, STEPS) and at(wk, 0xC04D) == 0x10 and at(wk, 0xC801) == 0,
      f"{state(wk, STEPS)} {state(wb, STEPS)}")
out, _, _, _ = run("play", 800, START + ["560:564:t"], extra=["-n", "500:1"])
out0, _, _, _ = run("play0", 800, START + ["560:564:t"])
check("3e the step sounds: START plays it", peak(out) >= 2000 and peak(out0) == 0, f"{peak(out)} {peak(out0)}")
# keys where there is no step row: the title, the top menu, PICK A PAT, the PATTERNS page
for name, ks, n in (("title", [], "200:3"),
                    ("menu", START + ["400:404:a"], "450:3"),
                    ("pick", START + ["400:404:s"], "450:3"),
                    ("patterns", START + presses(["a", "r", "r", "r", "r", "a"])[0], "520:3")):
    _, _, w, _ = run("guard_" + name, 600, ks, extra=["-n", n])
    _, _, wn, _ = run("guard0_" + name, 600, ks)
    check(f"3f a key on {name}: nothing changes", w and w == wn, "the work RAM differs")
# B held by the owner when the key goes down: the step waits for B to be up
_, _, w, _ = run("bheld", 600, START + ["480:540:b"], extra=["-n", "500:3"])
check("3g B held: the step is made after it is released", at(w, 0xC04D) == 0x04)
# another DMGo (another global checksum: the header checksum does not cover it): runs, no step keys
other = tmp / "other.gb"
data = bytearray(Path(rom).read_bytes())
data[0x14E] ^= 0xFF
other.write_bytes(bytes(data))
args = [host, other, "560", "-W", tmp / "other.wram", "-n", "500:3", "-k", START[0]]
p = subprocess.run(args, capture_output=True, text=True)
w = (tmp / "other.wram").read_bytes() if (tmp / "other.wram").exists() else b""
check("3h another version: runs, the white keys do nothing", p.returncode == 0 and w and at(w, 0xC04D) == 0,
      p.stdout + p.stderr)

print("dmgo_test: " + ("all passed" if not fails else f"{fails} FAILED"))
sys.exit(1 if fails else 0)
