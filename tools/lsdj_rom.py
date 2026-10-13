#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The Mac build's cartridge block: the owner's own LSDj ROM and its start-up save, as the firmware reads them
(firmware/src/lsdj/lsdj_cart.c) at the end of the app's flash slot. The install page (web/lsdj_install.html) does
the same in the browser, with the same C code, and must give the same bytes.

  lsdj_rom.py ROM build/lsdj/cart.bin MAX      MAX: the bytes free in the slot

LSDj's licence forbids copying, publishing or distributing it. The block, and every firmware with it in, are
private: the output must be git-ignored (checked), and nothing made from the ROM is ever committed or published.

The ROM: a Game Boy MBC5 + RAM + battery cartridge (0x1B) with a 128 KiB save (0x04), a valid header checksum,
a whole number of 16 KiB banks up to 1 MiB; anything else is refused (tools/lsdj_cart_tool.c). Each bank is stored
without its trailing 0xFF bytes (LSDj 9.4.2: 34 of 64 banks hold data, ~464 KB).

The start-up save: LSDj checks a blank save on its first start (~40 s at full speed). The host harness
(tests/lsdj/lsdj_host.c, the firmware's own emulator code) runs the ROM past it from a blank save; that save,
packed, is what the firmware starts from until one is saved to flash. With a demo song in it (tools/lsdj_demo.py,
typed into LSDj by the same harness; LSDJ_DEMO=0: an empty song): START plays it."""
import os
import re
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
BOOT_FRAMES = 3000                       # 50 s of Game Boy time: past the first-start check (~40 s)


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
        return sav
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
    return demo


def main(rom_path, out, room):
    out = Path(out).resolve()
    ign = subprocess.run(["git", "-C", SRC, "check-ignore", "-q", out], capture_output=True)
    if ign.returncode:
        raise SystemExit(f"lsdj_rom: {out} is not git-ignored: the ROM must never be committed")
    out.parent.mkdir(parents=True, exist_ok=True)
    tool = SRC / "build" / "host" / "lsdj_cart_tool"
    tool.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", f"-I{SRC / 'firmware/src'}", "-o", tool,
                    SRC / "tools" / "lsdj_cart_tool.c"], check=True)
    blank = out.parent / "blank.sav"                 # the ROM's own checks first: a block from an empty save
    blank.write_bytes(bytes(128 * 1024))
    r = subprocess.run([tool, rom_path, blank, out, str(1 << 21)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr.strip() or "lsdj_rom: lsdj_cart_tool failed")
    sav = boot_save(rom_path, out.parent)
    r = subprocess.run([tool, rom_path, sav, out, str(int(room, 0))], capture_output=True, text=True)
    if r.returncode:
        out.unlink(missing_ok=True)
        raise SystemExit(r.stderr.strip() or "lsdj_rom: lsdj_cart_tool failed")
    print(f"lsdj_rom: {r.stdout.strip()} -> {out}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*sys.argv[1:4])
