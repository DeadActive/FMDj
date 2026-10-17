#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The project is named FMDj (FMDJ on the device and the bezel); LSDj stays where it means the program or its ROM."""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2]
fails = 0


def check(name, cond):
    global fails
    print(("ok      " if cond else "FAILED  ") + name)
    fails += not cond


def text(p):
    return (SRC / p).read_text(encoding="utf-8")


pages = ["web/lsdj.html", "web/lsdj_install.html", "web/sim/index.html"]
docs = ["docs/lsdj/" + n for n in ("CONTROLS.md", "DEVICE_STEPS_INSTALL.md", "DEVICE_STEPS_SAVE.md",
                                   "DEVICE_STEPS_SOUND.md", "DEVICE_STEPS_TRANSFER.md", "SIMULATOR.md",
                                   "SOUND_TIMING.md")]
for p in pages + docs:
    t = text(p)
    check(p + " says FMDj", "FMDj" in t)
    check(p + " no longer says 'LSDj on the FM-1'", "LSDj on the FM-1" not in t)
for p in pages:
    m = re.search(r"<title>(.*?)</title>", text(p), re.S)
    check(p + " title starts with FMDj", bool(m) and m.group(1).strip().startswith("FMDj"))
check("the bezel print is FMDJ · FM-1", "FMDJ · FM-1" in text("web/sim/panel.js")
      and "LSDJ · FM-1" not in text("web/sim/panel.js"))
check("the start screen draws FMDJ", '"FMDJ"' in text("firmware/src/core_main.c"))
sim_c = text("web/sim/lsdjsim.c")
check("the simulator's start screen draws FMDJ, not LSDJ", '"FMDJ"' in sim_c and '"LSDJ"' not in sim_c)
check("the simulator page still credits LSDj's author", "LSDj is by Johan" in text("web/sim/index.html"))
check("the firmware still says NO LSDJ ROM", "NO LSDJ ROM" in text("firmware/src/lsdj/lsdj.c"))
check("the landing page trademark sentence", "trademarks of their respective owners" in text("web/fmdj/index.html")
      and "their owner's names" not in text("web/fmdj/index.html"))
check("SIMULATOR.md puts the greens in panel.js", "`panel.js`" in text("docs/lsdj/SIMULATOR.md").split("greens")[1][:20])
check("transfer steps name the FMDj start screen", "the FMDj start screen (FMDJ)" in text("docs/lsdj/DEVICE_STEPS_TRANSFER.md"))
check("the replace-all confirm names LSDj's songs", "Replace all of LSDj's songs on the FM-1 with" in text("web/lsdj.html"))
sys.exit(1 if fails else 0)
