#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI test: 2026 DEADACTIVE
"""Gate 4: no Bluetooth start-up reachable at boot. The call graph from fm1_cstart is walked through felucca.dis;
reaching any START function is an error, except through ALLOWED (the main loop's key handler)."""
import re
import sys

START = {"ble_start", "btstack_init", "btctrler_task_init", "bt_ble_init", "bt_master_ble_init"}
ALLOWED = {"ble_key"}                  # ble_ui.c: the PLAY handler; the only way in
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*$")
CALL = re.compile(r"call -?\d+ <([^:>+]+)")


def graph(text):
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


def main(dis):
    g = graph(open(dis).read())
    seen, todo, bad = set(), ["fm1_cstart"], []
    while todo:
        f = todo.pop()
        if f in seen or f in ALLOWED:
            continue
        seen.add(f)
        if f in START:
            bad.append(f)
        todo.extend(g.get(f, ()))
    if bad:
        print("BT start-up reachable at boot:", ", ".join(sorted(bad)))
        return 1
    print("no BT start-up reachable at boot (only through", ", ".join(sorted(ALLOWED)) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
