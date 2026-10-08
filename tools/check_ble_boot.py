#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI test: 2026 DEADACTIVE
"""Gate 4: no Bluetooth start-up reachable at boot. The call graph from fm1_cstart is walked through felucca.dis;
reaching any START function is an error, except through ALLOWED (the main loop's key handler).

  check_ble_boot.py build/felucca.dis           the BLE test build
  check_ble_boot.py build/felucca.dis --lsdj    the LSDj build: the Game Boy only after PLAY (lsdj.c)
  check_ble_boot.py --selftest"""
import re
import sys

START = {"ble_start", "btstack_init", "btctrler_task_init", "bt_ble_init", "bt_master_ble_init"}
LSDJ_START = {"lsdj_start", "lsdj_step", "lsdj_push", "emu_init", "emu_frame", "gb_init", "gb_run_frame",
              "__gb_step_cpu"}
ALLOWED = {"ble_key"}                  # ble_ui.c: the PLAY handler; the only way in
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*$")
CALL = re.compile(r"call -?\d+ <([^:>+]+)")
# a conditional call is printed on two lines: `if (...) {` / `call -486` / `} <callee : addr >`: join them
COND_CALL = re.compile(r"(call -?\d+)[ \t]*\n[ \t]*\} <")


def graph(text):
    text = COND_CALL.sub(r"\1 <", text)
    g, name = {}, None
    for line in text.splitlines():
        m = LABEL.match(line)
        if m:
            name = m.group(1)
            g[name] = set()
        elif name:
            for c in CALL.findall(line):
                g[name].add(c.strip())
    return g


def main(dis, start=START):
    g = graph(open(dis).read())
    seen, todo, bad = set(), ["fm1_cstart"], []
    while todo:
        f = todo.pop()
        if f in seen or f in ALLOWED:
            continue
        seen.add(f)
        if f in start:
            bad.append(f)
        todo.extend(g.get(f, ()))
    what = "Game Boy" if start is LSDJ_START else "BT start-up"
    if bad:
        print(f"{what} reachable at boot:", ", ".join(sorted(bad)))
        return 1
    if start is LSDJ_START and not start & set(g):
        print(f"none of {sorted(start)} in this build: nothing checked")
        return 1
    print(f"no {what} reachable at boot (only through", ", ".join(sorted(ALLOWED)) + ")")
    return 0


def selftest():
    import tempfile
    boot = "fm1_cstart:\n 0:\t00 00 \tcall -2 <fm1_main : 2000000 >\nfm1_main:\n"
    cases = [  # (disassembly after fm1_main's label, expected exit)
        (" 0:\t00 00 \tcall -2 <ble_key : 2000000 >\nble_key:\n 0:\t00 00 \tcall -2 <lsdj_start : 2000000 >\n"
         "lsdj_start:\n", 0),                                                     # only through the key handler
        (" 0:\t00 00 \tcall -2 <ble_service : 2000000 >\nble_service:\n 0:\t00 00 \tcall r0\nlsdj_step:\n", 0),
        (" 0:\t00 00 \tcall -2 <lsdj_step : 2000000 >\nlsdj_step:\n", 1),            # a direct call
        (" 0:\t10 ea \tif ((r0 & r4) != 0) {\n 2:\tbf ea \t\tcall -486\n          \t} <lsdj_start : 2007ee4 >\n"
         "lsdj_start:\n", 1),                                                     # a conditional call
    ]
    bad = 0
    for i, (body, want) in enumerate(cases):
        with tempfile.NamedTemporaryFile("w", suffix=".dis", delete=False) as f:
            f.write(boot + body)
        got = main(f.name, LSDJ_START)
        if got != want:
            print(f"selftest case {i}: exit {got}, expected {want}")
            bad += 1
    print("check_ble_boot selftest:", "FAILED" if bad else "ok")
    return 1 if bad else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    sys.exit(main(sys.argv[1], LSDJ_START if "--lsdj" in sys.argv[2:] else START))
