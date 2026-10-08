#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The LSDj unit's private data: the owner's own LSDj ROM and the start-up save, as a C header.

  lsdj_rom.py ROM build/lsdj/lsdj_rom.h

LSDj's licence forbids copying, publishing or distributing it. The header, and every firmware built with it, are
private: the output must be git-ignored (checked), and nothing made from the ROM is ever committed or published.

The ROM: a Game Boy MBC5 + RAM + battery cartridge (0x1B) with a 128 KiB save (0x04), a valid header checksum,
a whole number of 16 KiB banks up to 1 MiB; anything else is refused. Each bank is stored without its trailing 0xFF
bytes (LSDj 9.4.2: 34 of 64 banks hold data, ~464 KB).

The start-up save: LSDj checks a blank save on its first start (~40 s at full speed). The host harness
(tests/lsdj/lsdj_host.c, the firmware's own emulator code) runs the ROM past it from a blank save; that save,
PackBits-packed, is what the firmware starts from (all in RAM: nothing is saved to flash). With a demo song in it
(tools/lsdj_demo.py, typed into LSDj by the same harness; LSDJ_DEMO=0: an empty song): START plays it."""
import os
import re
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
BANK = 0x4000
BOOT_FRAMES = 3000                       # 50 s of Game Boy time: past the first-start check (~40 s)


def check_rom(rom):
    if not rom or len(rom) % BANK or len(rom) > 64 * BANK:
        raise SystemExit(f"lsdj_rom: {len(rom)} B is not a whole number of 16 KiB banks up to 1 MiB")
    if rom[0x147] != 0x1B or rom[0x149] != 0x04:
        raise SystemExit(f"lsdj_rom: cartridge {rom[0x147]:#04x} / RAM {rom[0x149]:#04x}: not MBC5 + RAM + battery, "
                         "128 KiB (LSDj)")
    x = 0
    for b in rom[0x134:0x14D]:
        x = (x - b - 1) & 0xFF
    if x != rom[0x14D]:
        raise SystemExit("lsdj_rom: header checksum mismatch")


def banks(rom):
    out = []
    for i in range(0, len(rom), BANK):
        b = rom[i:i + BANK].rstrip(b"\xff")
        out.append(b)
    return out


def packbits(d):
    o, i = bytearray(), 0
    while i < len(d):
        j = i
        while j < len(d) and j - i < 128 and d[j] == d[i]:
            j += 1
        if j - i >= 3:
            o += bytes([0x80 | (j - i - 1), d[i]])
            i = j
            continue
        k = i
        while k < len(d) and k - i < 128 and not (k + 2 < len(d) and d[k] == d[k + 1] == d[k + 2]):
            k += 1
        o += bytes([k - i - 1]) + d[i:k]
        i = k
    return bytes(o)


def unpack(p):                           # the firmware's lsdj_unpack, for the round-trip check
    o, i = bytearray(), 0
    while i < len(p):
        c = p[i]
        i += 1
        if c < 128:
            o += p[i:i + c + 1]
            i += c + 1
        else:
            o += bytes([p[i]]) * ((c & 127) + 1)
            i += 1
    return bytes(o)


def boot_save(rom_path, out_dir):
    host = SRC / "build" / "host" / "lsdj_host"
    host.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-w", f"-I{SRC / 'firmware/src'}", f"-I{SRC / 'firmware/src/lsdj'}", "-o", host,
                    SRC / "tests" / "lsdj" / "lsdj_host.c"], check=True)
    sav = out_dir / "boot.sav"
    r = subprocess.run([host, rom_path, str(BOOT_FRAMES), "-o", sav, "-p", out_dir / "boot.pgm"],
                       capture_output=True, text=True)
    print("lsdj_rom: host run:", r.stdout.strip() or r.stderr.strip())
    if r.returncode:
        raise SystemExit("lsdj_rom: the ROM did not run on the host emulator")
    if os.environ.get("LSDJ_DEMO") == "0":
        return sav.read_bytes()
    sys.path.insert(0, str(SRC / "tools"))
    import lsdj_demo
    args, end = lsdj_demo.key_args(60)
    demo = out_dir / "demo.sav"
    r = subprocess.run([host, rom_path, str(end + 60), "-s", sav, "-o", demo, "-p", out_dir / "demo.pgm", *args],
                       capture_output=True, text=True)
    print("lsdj_rom: demo typed:", r.stdout.strip() or r.stderr.strip())
    if r.returncode:
        raise SystemExit("lsdj_rom: typing the demo song failed")
    r = subprocess.run([host, rom_path, "600", "-s", demo, "-k", "60:64:t", "-w", out_dir / "demo.wav"],
                       capture_output=True, text=True)        # START: the demo must play
    peak = re.search(r"audio peak (\d+)", r.stdout)
    print("lsdj_rom: demo played:", r.stdout.strip() or r.stderr.strip())
    if r.returncode or not peak or int(peak.group(1)) < 2000:
        raise SystemExit("lsdj_rom: the demo song does not play")
    return demo.read_bytes()


def c_bytes(name, data):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    return f"static const uint8_t {name}[{max(len(data), 1)}] = {{\n" + ",\n".join(rows or ["0"]) + "\n};\n"


def main(rom_path, out):
    out = Path(out).resolve()
    ign = subprocess.run(["git", "-C", SRC, "check-ignore", "-q", out], capture_output=True)
    if ign.returncode:
        raise SystemExit(f"lsdj_rom: {out} is not git-ignored: the ROM must never be committed")
    rom = Path(rom_path).read_bytes()
    check_rom(rom)
    bk = banks(rom)
    out.parent.mkdir(parents=True, exist_ok=True)
    sav = boot_save(rom_path, out.parent)
    packed = packbits(sav)
    if unpack(packed) != sav or len(sav) != 128 * 1024:
        raise SystemExit("lsdj_rom: start-up save packing failed")
    parts = ["/* PRIVATE: made from the owner's LSDj ROM by tools/lsdj_rom.py. Never commit or publish. */\n"]
    for i, b in enumerate(bk):
        if b:
            parts.append(c_bytes(f"lsdj_bank{i}", b))
    parts.append(f"#define LSDJ_ROM_BANKS {len(bk)}u\nstatic const lsdj_bank_t lsdj_rom[LSDJ_ROM_BANKS] = {{\n")
    parts.append(",\n".join(f"    {{lsdj_bank{i}, {len(b)}u}}" if b else "    {0, 0}" for i, b in enumerate(bk)))
    parts.append("\n};\n")
    parts.append(c_bytes("lsdj_boot_save", packed))
    out.write_text("".join(parts))
    print(f"lsdj_rom: {sum(len(b) for b in bk)} B in {sum(1 for b in bk if b)} of {len(bk)} banks, "
          f"start-up save {len(packed)} B packed -> {out}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
