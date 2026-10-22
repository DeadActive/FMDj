#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The FMDj landing page (web/fmdj/, written by web/make_site.py as the site's root index.html): built from the
private package with made-up screenshots of both programs (FMDJ_SHOTS: LSDj's, FMDJ_DMGO_SHOTS: DMGo's), of one,
and with none. Checks its name, its links, that every local href / src / import on it is a file of the site, the
galleries with shots and no shots/ reference without them, its sections (the emulator, LSDj, DMGo, how to use it, the
roadmap) and the credits.
Also runs the simulator's node test (web/sim/panel.js came out of web/sim/app.js). Skipped without
build/lsdj-private.fwsc and the WebAssembly (build/web/sim/lsdjsim.wasm, build/web/lsdj_emu.wasm)."""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit

SRC = Path(__file__).resolve().parents[2]
PKG = SRC / "build" / "lsdj-private.fwsc"
NEEDS = (PKG, SRC / "build" / "web" / "sim" / "lsdjsim.wasm", SRC / "build" / "web" / "lsdj_emu.wasm")
OUT = SRC / "build" / "tests" / "fmdj_site"
fails = 0


def check(name, cond, extra=""):
    global fails
    print(("ok      " if cond else "FAILED  ") + name + (" " + str(extra) if extra and not cond else ""))
    fails += not cond


def png(w, h, rgb):
    """a tiny valid PNG of one colour"""
    def chunk(typ, body):
        return struct.pack(">I", len(body)) + typ + body + struct.pack(">I", zlib.crc32(typ + body))
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class Page(HTMLParser):
    """the page's title, its h1 text, every href / src, the text"""
    def __init__(self):
        super().__init__()
        self.title, self.h1, self.refs, self.text, self._in = "", "", [], "", []

    def handle_starttag(self, tag, attrs):
        self._in.append(tag)
        self.refs += [v for k, v in attrs if k in ("href", "src") and v is not None]

    def handle_endtag(self, tag):
        while self._in and self._in.pop() != tag:
            pass

    def handle_data(self, data):
        self.text += data
        if "title" in self._in:
            self.title += data
        if "h1" in self._in:
            self.h1 += data


def local(ref):
    u = urlsplit(ref)
    return not (u.scheme or u.netloc or ref.startswith("#") or ref.startswith("//"))


def resolves(site, base, ref):
    """a local reference from the file in directory `base` -> a file or directory of the site"""
    path = urlsplit(ref).path
    target = (base / path).resolve()
    return target.is_relative_to(site.resolve()) and (target.is_file() or (target.is_dir() and
                                                                            (target / "index.html").is_file()))


def imports(js):
    return re.findall(r"""^\s*import\b[^;]*?from\s+['"]([^'"]+)['"]""", js, flags=re.M) + \
        re.findall(r"""import\(\s*['"]([^'"]+)['"]\s*\)""", js)


def build(case, lsdj, dmgo, site=None, ok=True):
    site = site or OUT / case
    if site.parent == OUT:
        shutil.rmtree(site, ignore_errors=True)
    env = dict(os.environ, FMDJ_SHOTS=str(lsdj), FMDJ_DMGO_SHOTS=str(dmgo))
    r = subprocess.run([sys.executable, str(SRC / "web" / "make_site.py"), str(PKG), "test", str(site)],
                       cwd=SRC, env=env, capture_output=True, text=True)
    if ok:
        check(f"{case}: make_site runs", r.returncode == 0, r.stderr[-800:])
    return site, r


def shots_dir(name, prog, names):
    d = OUT / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    man = [{"prog": prog, "name": n, "title": n.upper().replace("-", " "), "file": f"{n}.png"} for n in names]
    for i, m in enumerate(man):
        (d / m["file"]).write_bytes(png(4, 3, (0x9B - i * 20, 0xBC - len(prog), 0x0F)))
    (d / "manifest.json").write_text(json.dumps(man))
    return d, man


def section(html, sid):
    """the markup of <section id=sid> up to the next section (or the footer)"""
    m = re.search(r'<section\b[^>]*\bid="%s"[^>]*>(.*?)(?=<section\b|<footer\b)' % sid, html, re.S)
    return m.group(1) if m else ""


def common(case, site):
    index = site / "index.html"
    html = index.read_text(encoding="utf-8") if index.exists() else ""
    p = Page()
    p.feed(html)
    check(f"{case}: <title> has FMDj", "FMDj" in p.title, repr(p.title))
    check(f"{case}: a heading with FMDj", "FMDj" in p.h1, repr(p.h1))
    hrefs = {urlsplit(r).path for r in p.refs}
    for link in ("webapp/lsdj-install/", "webapp/lsdj/", "webapp/lsdj-sim/"):
        check(f"{case}: links {link}", link in hrefs)
    bad = [r for r in p.refs if local(r) and not resolves(site, site, r)]
    check(f"{case}: every local href / src is a file of the site ({len([r for r in p.refs if local(r)])})", not bad, bad)
    check(f"{case}: no redirect", "http-equiv" not in html.lower())
    scripts = [r for r in p.refs if r.endswith(".js") and local(r)]
    check(f"{case}: the page's script is there", bool(scripts), p.refs)
    seen, todo, badimp = set(), [(site / s).resolve() for s in scripts + ["webapp/lsdj-sim/app.js"]], []
    while todo:                                     # every module the page (and the simulator) loads, and theirs
        f = todo.pop()
        if f in seen or not f.is_file():
            continue
        seen.add(f)
        for spec in imports(f.read_text(encoding="utf-8")):
            if not resolves(site, f.parent, spec):
                badimp.append(f"{f.relative_to(site)}: {spec}")
            else:
                todo.append((f.parent / spec).resolve())
    check(f"{case}: every import of its modules and the simulator's is a file of the site ({len(seen)} modules)",
          not badimp and any(f.name == "panel.js" for f in seen) and any(f.name == "app.js" for f in seen),
          badimp or sorted(str(f) for f in seen))
    return html, p


if any(not f.exists() for f in NEEDS):
    print("skipped: needs " + ", ".join(str(f.relative_to(SRC)) for f in NEEDS if not f.exists()))
    sys.exit(0)

# ---- with shots of both: three tiny LSDj PNGs, two DMGo ones, their manifests
lsdj, lman = shots_dir("shots-lsdj", "lsdj", ("song", "chain", "phrase"))
dmgo, dman = shots_dir("shots-dmgo", "dmgo", ("title", "ch1-pulse"))
empty = OUT / "shots-none"
shutil.rmtree(empty, ignore_errors=True)
empty.mkdir(parents=True)
site, _ = build("with", lsdj, dmgo)
html, p = common("with", site)
for prog, who, src, man in (("lsdj", "LSDj", lsdj, lman), ("dmgo", "DMGo", dmgo, dman)):
    for m in man:
        f = f"shots/{prog}/{m['file']}"
        check(f"with: the page names {f}", f in html)
        check(f"with: {f} is the one given", (site / f).is_file() and (site / f).read_bytes() == (src / m["file"]).read_bytes())
        check(f"with: the page names {who}'s screen {m['title']}", m["title"] in p.text
              and f"{who}'s {m['title']} screen" in html)
    check(f"with: shots/{prog}/manifest.json", (site / "shots" / prog / "manifest.json").is_file()
          and json.loads((site / "shots" / prog / "manifest.json").read_text()) == man)
gal = section(html, "shots")
check("with: two galleries, each under its program's name, LSDj's first",
      re.search(r"<h3[^>]*>LSDj</h3>.*?shots/lsdj/.*?<h3[^>]*>DMGo!</h3>.*?shots/dmgo/", gal, re.S) is not None
      and "shots/dmgo/" not in gal.split("DMGo!</h3>")[0])

# ---- the sections and what they say (the page as built, with shots)
for sid, words in (("emulator", ("Peanut-GB", "minigb_apu", "within the frame", "FPS", "CPU", "GAPS")),
                   ("lsdj", ("Johan Kotlinski", "littlesounddj.com")),
                   ("dmgo", ("Audiowanderer", "free", "white keys", "16")),
                   ("how", ("install page", "Chrome", "Edge", "USB", "PLAY", "D#5", "F#5", "GLO", "REC",
                            "song page", "simulator", "SELECT knob", "ALGORITHM knob")),
                   ("roadmap", ("link port", "MIDI clock", "mGB"))):
    body = section(html, sid)
    check(f"section {sid}: there, with a heading", re.match(r"\s*<h2\b", body) is not None, body[:200])
    miss = [w for w in words if w not in body]
    check(f"section {sid}: says {', '.join(words)}", not miss, miss)
check("DMGo's page linked", 'href="https://audiowanderer.com/AW/drum-machina-go-or/"' in html)
check("LSDj's page linked", 'href="https://www.littlesounddj.com/"' in html)
foot = html[html.find("<footer"):]
for words in ("DEADACTIVE", "Johan Kotlinski", "Audiowanderer", "Peanut-GB", "minigb_apu", "Leo Kuroshita",
              "M-VAVE and FM-1 are trademarks of their respective owners.", "Game Boy is a trademark of Nintendo",
              "firmware/LICENSES/", "GPL-3.0"):
    check(f"the credits: {words}", words in foot)
check("no stale names (the status line's SPD / LOAD, SEL / SAVE)", not re.search(r"\bSPD\b|\bLOAD\b|SEL / SAVE", p.text))
check("the how-to names both programs' controls", "LSDj" in section(html, "how") and "DMGo" in section(html, "how"))

# ---- one program's shots only: that gallery, no reference to the other's
for case, l, d, has, hasnt in (("lsdj-only", lsdj, empty, "lsdj", "dmgo"), ("dmgo-only", empty, dmgo, "dmgo", "lsdj")):
    site, _ = build(case, l, d)
    html, p = common(case, site)
    check(f"{case}: shots/{has}/ on the page, no shots/{hasnt}/", f"shots/{has}/" in html and f"shots/{hasnt}/" not in html)
    check(f"{case}: no shots/{hasnt} directory", (site / "shots" / has).is_dir() and not (site / "shots" / hasnt).exists())

# ---- without shots: empty directories
site, _ = build("without", empty, empty)
html, p = common("without", site)
check("without: no shots/ reference on the page", "shots/" not in html)
check("without: no shots/ directory", not (site / "shots").exists())

# ---- a site built over one with shots loses them (no stale pictures)
site, r = build("rebuilt without", empty, empty, site=OUT / "with")
check("rebuilt without: the old shots are gone", r.returncode == 0 and not (site / "shots").exists()
      and "shots/" not in (site / "index.html").read_text(encoding="utf-8"))

# ---- one program's screens given as the other's: refused
_, r = build("swapped", dmgo, empty, site=OUT / "swapped", ok=False)
check("DMGo's manifest as LSDj's (FMDJ_SHOTS): refused, naming it", r.returncode != 0 and "dmgo" in r.stdout + r.stderr,
      r.stdout + r.stderr)

# ---- shots are not copied into an output that lies inside another git repository
foreign = OUT / "foreign-repo"
shutil.rmtree(foreign, ignore_errors=True)
foreign.mkdir(parents=True)
subprocess.run(["git", "init", "-q", str(foreign)], check=True)
site, r = build("foreign", lsdj, dmgo, site=foreign / "site")
check("foreign repository: no shots copied, and says why",
      r.returncode == 0 and not (site / "shots").exists() and "repository" in r.stdout, r.stdout + r.stderr)

# ---- the simulator after its panel moved to panel.js
r = subprocess.run(["node", str(SRC / "tests" / "lsdj" / "lsdj_sim_test.mjs")], cwd=SRC, capture_output=True, text=True)
check("simulator: node tests/lsdj/lsdj_sim_test.mjs passes", r.returncode == 0, (r.stdout + r.stderr)[-800:])
for f in ("web/sim/app.js", "web/sim/panel.js", "web/fmdj/fmdj.js"):
    r = subprocess.run(["node", "--check", str(SRC / f)], capture_output=True, text=True)
    check(f"node --check {f}", r.returncode == 0, r.stderr[-400:])

print("ALL PASSED" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
