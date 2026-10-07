#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI test: 2026 DEADACTIVE
"""Gate 2: the update path's machine code equals the stripped core's reference.

  check_update_path.py build/felucca.dis tests/ble/update_path.ref          compare (exit 1 on a difference)
  check_update_path.py build/felucca.dis tests/ble/update_path.ref --write  write the reference (core build only)
  check_update_path.py --selftest

Functions compared: every function whose name matches PATTERNS, plus everything in .ram_text (the flash
driver). Instructions are compared as text with addresses and encodings dropped and branch / load targets
normalised to symbol names, so moving code around is fine and changing it is not."""
import re
import sys

PATTERNS = [r"^usb_", r"^ep[0-4]_", r"^e0_", r"^sie_", r"^sysex_byte$", r"^midi_in_event$", r"^midi_enqueue$",
            r"^ota_", r"^fm1_timer5_irq$", r"^fm1_cstart$", r"^fl_", r"^st_", r"^uac_"]
# (not cdc_task: the console's commands inline into it and change with them; the CDC transport is in usb_poll)
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*$")
INSN = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} )+\s*\t(.*)$")
COMMENT = re.compile(r"\s*#\s*<[^>]*>")          # objdump's "# <sym : addr>" notes after a constant
ANNOT = re.compile(r"(-?\d+) <([^:>]+?)\s*:\s*([0-9a-f]+)\s*>", re.I)


def normalise(text, fn):
    """`N <sym+off : addr>` operands, independent of where things were placed: a call keeps its callee's name; a
    branch inside the function keeps its offset; any other RAM / XIP address is a data address, `<addr>` (objdump
    names the same address differently from build to build, e.g. _bss_end / an empty pool after it); anything else
    is a plain constant objdump annotated with the nearest symbol, and stays the number N"""
    def one(m):
        num, sym, addr = m.groups()
        sym = sym.strip()
        if not 0x01C00000 <= int(addr, 16) < 0x02100000:
            return num
        if text.lstrip().startswith("call"):
            return f"<{sym.split('+')[0]}>"
        if sym == fn or sym.startswith(fn + "+"):
            return f"<{sym[len(fn):] or '+0'}>"
        return "<addr>"
    return ANNOT.sub(one, COMMENT.sub("", text)).strip()


def functions(text):
    out, name = {}, None
    for line in text.splitlines():
        m = LABEL.match(line)
        if m:
            name = m.group(1)
            out[name] = []
            continue
        m = INSN.match(line)
        if m and name:
            out[name].append(normalise(m.group(1), name))
        elif line.startswith("| ") and name:      # a reference file's instruction line (render)
            out[name].append(line[2:].rstrip("\n"))
    return out


def selected(funcs):
    return {n: body for n, body in funcs.items() if any(re.search(p, n) for p in PATTERNS)}


def render(funcs):
    return "".join(f"{n}:\n" + "".join(f"| {i}\n" for i in funcs[n]) for n in sorted(funcs))


def compare(dis_text, ref_text):
    now, ref = selected(functions(dis_text)), functions(ref_text)
    errors = [f"missing in this build: {n}" for n in ref if n not in now]
    errors += [f"changed: {n}" for n in ref if n in now and now[n] != ref[n]]
    errors += [f"new update-path function (not in the reference): {n}" for n in now if n not in ref]
    return errors


def selftest():
    dis = ("usb_poll:\n  2000000:    00 49    \tr0 = 1\n  2000002:    80 00    \trts\n"
           "other:\n  2000010:    80 00    \trts\n")
    ref = render(selected(functions(dis)))
    assert compare(dis, ref) == [], "identical listing flagged"
    assert compare(dis.replace("r0 = 1", "r0 = 2"), ref), "changed instruction not caught"
    assert compare(dis.replace("usb_poll:", "usb_poll2:"), ref), "missing function not caught"
    moved = dis.replace("2000000:", "2000100:")
    assert compare(moved, ref) == [], "moved code flagged"
    # objdump's real forms: upper-case offsets, relative call / branch numbers that change with layout, constants
    # annotated with a nonsense nearest symbol (keep the constant), address constants (normalise to the symbol)
    a = ("usb_x:\n  2000000:    00 49    \tif (r1 == 0) goto 38 <usb_x+0x2A : 2002d6c >\n"
         "  2000002:    00 49    \tcall -40632 <memset : 2001fae >\n"
         "  2000004:    00 49    \tr3 = -20000 <_data_load+0xFFFFFFFFFDFEADC8 : ffffffffffffb1e0 >\n"
         "  2000006:    00 49    \tr0 = 33720110 <usb_x.D1 : 202872e >\n")
    b = (a.replace("2002d6c", "2003faa").replace("-40632", "-50000").replace("2001fae", "2003fae")
          .replace("FFFFFFFFFDFEADC8", "FFFFFFFFFDF84D5C").replace("33720110", "33800000").replace("202872e", "2035f00"))
    ref2 = render(selected(functions(a)))
    assert compare(b, ref2) == [], "layout-only changes flagged"
    assert compare(b.replace("-20000", "-20004"), ref2), "a changed constant not caught"
    assert compare(b.replace("<memset", "<memcpy"), ref2), "a changed call target not caught"
    c1 = "usb_y:\n  2000000:    00 49    \tr1 = -12711  # <_data_load+0xFFFFFFFFFDFECA40 : ffffffffffffce58 >\n"
    ref3 = render(selected(functions(c1)))
    assert compare(c1.replace("FDFECA40", "FDF869D4"), ref3) == [], "a comment annotation flagged"
    assert compare(c1.replace("-12711", "-12712"), ref3), "the constant before a comment not caught"
    # the same address may carry several names (an empty pool after _bss_end): addresses compare by role, branches
    # inside the function by offset
    d1 = ("usb_z:\n  2000000:    00 49    \tr3 = 29410364 <_bss_end : 1c0c43c >\n"
          "  2000002:    00 49    \tgoto 10 <usb_z+0x2A : 200002a >\n")
    ref4 = render(selected(functions(d1)))
    assert compare(d1.replace("_bss_end", "tws_bulk_pool_end"), ref4) == [], "an address alias flagged"
    assert compare(d1.replace("usb_z+0x2A", "usb_z+0x2C"), ref4), "a moved branch target not caught"
    print("check_update_path: selftest ok")


# gate 2a: the update path's sources (the loader's unit and the core's USB / update / flash code) are Felucca
# 1.0.5.2's, as on branch main
SOURCES = ["firmware/src/usb.c", "firmware/src/ota.c", "firmware/src/ota_hw.c", "firmware/src/libc.c",
           "firmware/hal/fm1_flash.h", "firmware/hal/fm1_sys.h", "firmware/hal/fm1_time.h", "firmware/hal/fm1_usb.h",
           "firmware/loader", "firmware/src/storage_hw.c", "firmware/src/storage.c"]


def sources(base):
    import subprocess
    r = subprocess.run(["git", "diff", "--stat", base, "--", *SOURCES], capture_output=True, text=True)
    if r.returncode or r.stdout.strip():
        print("update path sources differ from", base + ":\n" + (r.stdout or r.stderr))
        return 1
    print(f"update path sources: identical to {base}")
    return 0


def main(argv):
    if argv[1:2] == ["--sources"]:
        return sources(argv[2] if len(argv) > 2 else "main")
    if argv[1:] == ["--selftest"]:
        selftest()
        return 0
    dis_text = open(argv[1]).read()
    if "--write" in argv:
        open(argv[2], "w").write(render(selected(functions(dis_text))))
        print(f"wrote {argv[2]}")
        return 0
    errors = compare(dis_text, open(argv[2]).read())
    for e in errors:
        print("update path:", e)
    print("update path: unchanged" if not errors else f"update path: {len(errors)} difference(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
