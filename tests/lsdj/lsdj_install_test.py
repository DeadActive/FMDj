#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The LSDj install page's logic (web/lsdjinstall.js with web/lsdj_wasm.c as WebAssembly, run in node) against the
Mac build (tools/lsdj_rom.py, tools/lsdj_cart_tool.c, tools/fm1pkg_make.py): from the same ROM both must make the
same cartridge block and the same package, byte for byte. The FM-1's loader writes the app without checking the
app's own checksum, so this is what stands between the page and an FM-1 that does not start.

  1. With a small Game Boy program of our own (made below; no LSDj data), put into this build's firmware.
  2. What the page refuses: a file that is no ROM, a ROM that draws nothing, one that does not fit, a package that
     is not the LSDj firmware without a ROM.
  3. With the owner's ROM (LSDJ_ROM, and a build made with it: build/lsdj/cart.bin, build/lsdj-base.fwsc,
     build/lsdj-private.fwsc): the page's block and package are the build's. Skipped without them.

Needs node, build/web/lsdj_emu.wasm (web/lsdj_wasm.sh) and one ./build.sh; skipped without them."""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SRC / "tools"))
T = SRC / "build" / "tests" / "install"
WASM = SRC / "build" / "web" / "lsdj_emu.wasm"
fails = 0


def ok(c, what):
    global fails
    print(f"{what:<78} {'ok' if c else 'FAILED'}")
    fails += not c


def test_rom(draw=True):
    """32 KiB, MBC5 + RAM + battery, a 128 KiB save: a timer interrupt 358 times a second, a note every 64th, a wait
    loop like LSDj's between them, and (draw) one dark row in every tile so the screen is not blank"""
    r = bytearray(b"\xff" * 0x8000)

    def at(a, *b):
        r[a:a + len(b)] = bytes(b)
    at(0x40, 0xF5, 0x3E, 0x01, 0xEA, 0x01, 0xC1, 0xF1, 0xD9)           # VBLANK: push af; ld a,1; ld (C101),a; pop af; reti
    at(0x50, 0xF5, 0x3E, 0x01, 0xEA, 0x00, 0xC1, 0xF1, 0xD9)           # timer: ... ld (C100),a ...
    at(0x100, 0x00, 0xC3, 0x50, 0x01)                                  # nop; jp 0150
    r[0x104:0x150] = bytes(0x4C)
    r[0x147], r[0x148], r[0x149] = 0x1B, 0x00, 0x04
    x = 0
    for b in r[0x134:0x14D]:
        x = (x - b - 1) & 0xFF
    r[0x14D] = x
    at(0x150,
       0xF3, 0x31, 0xFF, 0xDF,                                         # di; ld sp,DFFF
       0x3E, 0x80, 0xE0, 0x26, 0x3E, 0x77, 0xE0, 0x24, 0x3E, 0xFF, 0xE0, 0x25,   # NR52 = 80, NR50 = 77, NR51 = FF
       0x3E, 0xB0, 0xE0, 0x11, 0x3E, 0xF0, 0xE0, 0x12, 0x3E, 0x83, 0xE0, 0x13,   # NR11 = B0, NR12 = F0, NR13 = 83
       0x3E, 0x49, 0xE0, 0x06, 0xE0, 0x05, 0x3E, 0x06, 0xE0, 0x07,     # TMA = TIMA = 49; TAC = 06
       0xAF, 0xE0, 0x0F, 0xEA, 0x00, 0xC1, 0xEA, 0x01, 0xC1, 0xEA, 0x02, 0xC1,   # IF = 0; C100..C102 = 0
       *((0x21, 0x00, 0x80, 0x3E, 0xFF, 0x22, 0x77) if draw else (0x00,) * 7),   # ld hl,8000; ld a,FF; ld (hl+),a; ld (hl),a
       0x3E, 0x05, 0xE0, 0xFF, 0xFB, 0xC3, 0x00, 0x02)                 # IE = 05; ei; jp 0200
    at(0x200, 0xAF, 0x21, 0x00, 0xC1, 0xB6, 0xC2, 0x40, 0x02,          # the wait loop: xor a; ld hl,C100; or (hl); jp nz,0240
       0xAF, 0x21, 0x01, 0xC1, 0xB6, 0xCA, 0x00, 0x02)                 # xor a; ld hl,C101; or (hl); jp z,0200
    at(0x210, 0xAF, 0xEA, 0x01, 0xC1, 0xC3, 0x00, 0x02)                # a frame: C101 = 0; jp 0200
    at(0x240, 0xAF, 0xEA, 0x00, 0xC1, 0x21, 0x02, 0xC1, 0x34,          # a timer interrupt: C100 = 0; inc (C102)
       0x7E, 0xE6, 0x3F, 0xC2, 0x00, 0x02,                             # ld a,(hl); and 3F; jp nz,0200
       0x3E, 0xC6, 0xE0, 0x14, 0xC3, 0x00, 0x02)                       # every 64th: NR14 = C6 (the note); jp 0200
    return bytes(r)


def page(rom, base, demo, name):
    """the page's logic on these files -> (exit code, what it said, the block, the package)"""
    c, p = T / f"out-{name}.cart", T / f"out-{name}.fwsc"
    for f in (c, p):
        f.unlink(missing_ok=True)
    r = subprocess.run(["node", SRC / "web" / "lsdj_install_cli.mjs", WASM, rom, demo, base, c, p],
                       capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip(), c.read_bytes() if c.exists() else b"", \
        p.read_bytes() if p.exists() else b""


def main():
    if not shutil.which("node") or not WASM.exists() or not (SRC / "build" / "felucca.bin").exists():
        print("lsdj_install_test: skipped (needs node, build/web/lsdj_emu.wasm from web/lsdj_wasm.sh, and one ./build.sh)")
        return 0
    import fm1pkg_make
    import lsdj_demo
    import lsdj_rom
    T.mkdir(parents=True, exist_ok=True)
    args, end = lsdj_demo.key_args(60)
    demo = T / "demo.json"
    demo.write_text(json.dumps({"keys": [[int(a), int(b), k] for a, b, k in (x.split(":") for x in args[1::2])],
                                "frames": end + 60}))
    ota = (SRC / "build" / "loader" / "ota.bin").read_bytes()
    slot = fm1pkg_make.APP_SLOT

    def package(img):
        return fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, "FM-1_900")

    def marked(img):                                  # as tools/build.py ends an LSDj firmware
        return img + b"LSDE" + (len(img) + 8).to_bytes(4, "little")
    app = (SRC / "build" / "felucca.bin").read_bytes()
    if app.endswith(b"LSDE" + len(app).to_bytes(4, "little")):
        app = app[:-8]
    img = marked(app)
    base = T / "base.fwsc"
    base.write_bytes(package(img))

    # 1. our own program: the Mac's way, then the page's
    rom = T / "test.gb"
    rom.write_bytes(test_rom())
    mac = T / "mac.cart"
    lsdj_rom.main(str(rom), str(mac), str(slot - len(img)))
    blk = mac.read_bytes()
    rc, said, cart, pkg = page(rom, base, demo, "page")
    ok(rc == 0 and cart == blk and len(blk) > 100, f"our test program: the page's block is the Mac's ({len(blk)} B; {said})")
    ok(pkg == package(img + b"\xff" * (slot - len(img) - len(blk)) + blk), "... and its package is the Mac's, byte for byte")
    ok(len(pkg) == len(base.read_bytes()) and pkg != base.read_bytes(), "... the size of the firmware's own, and not it")

    # 2. refusals: nothing is written
    junk = T / "junk.gb"
    junk.write_bytes(bytes(range(256)) * 128)
    rc, said, cart, pkg = page(junk, base, demo, "junk")
    ok(rc == 1 and "not an LSDj ROM" in said and not cart and not pkg, f"a file that is no ROM: {said[:48]}")
    blank = T / "blank.gb"
    blank.write_bytes(test_rom(draw=False))
    rc, said, cart, pkg = page(blank, base, demo, "blank")
    ok(rc == 1 and "screen stays blank" in said and not pkg, f"a ROM that draws nothing: {said[:52]}")
    tight = T / "tight.fwsc"                          # a firmware that leaves 100 bytes
    tight.write_bytes(package(marked(app + bytes(slot - len(app) - 8 - 100))))
    rc, said, cart, pkg = page(rom, tight, demo, "tight")
    ok(rc == 1 and "too many" in said and "leaves 100" in said and not pkg, f"a ROM that does not fit: {said[:56]}")
    plain = T / "plain.fwsc"                          # a firmware without the end mark
    plain.write_bytes(package(app))
    rc, said, cart, pkg = page(rom, plain, demo, "plain")
    ok(rc == 1 and "not the LSDj firmware" in said and not pkg, f"a package that is not the LSDj base: {said[:44]}")
    rc, said, cart, pkg = page(rom, T / "out-page.fwsc", demo, "twice")       # a package that already holds a ROM
    ok(rc == 1 and "not the LSDj firmware" in said and not pkg, "a package with a ROM in it already")

    # 3. the owner's ROM, against the build made with it
    own = os.environ.get("LSDJ_ROM")
    B = SRC / "build"
    need = [B / "lsdj" / "cart.bin", B / "lsdj-base.fwsc", B / "lsdj-private.fwsc"]
    if not own or not all(f.exists() for f in need):
        print("the owner's ROM: skipped (LSDJ_ROM, and a build with it: FELUCCA_LSDJ=1 LSDJ_ROM=... ./build.sh)")
    else:
        rc, said, cart, pkg = page(own, need[1], demo, "own")
        ok(rc == 0 and cart == need[0].read_bytes(), f"the owner's ROM: the page's block is the build's ({said})")
        ok(pkg == need[2].read_bytes(), "... and its package is build/lsdj-private.fwsc, byte for byte")
        for f in (T / "out-own.cart", T / "out-own.fwsc"):    # (made from the ROM: not kept)
            f.unlink(missing_ok=True)
    print("lsdj_install_test: %s" % ("%d FAILED" % fails if fails else "all passed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
