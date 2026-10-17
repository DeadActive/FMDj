#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
"""The FMDj landing page (web/fmdj/, written by web/make_site.py as the site's root index.html): built from the
private package with made-up screenshots (FMDJ_SHOTS) and with none. Checks its name, its links, that every local
href / src / import on it is a file of the site, the gallery with shots and no shots/ reference without them.
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


def build(case, shots):
    site = OUT / case
    shutil.rmtree(site, ignore_errors=True)
    env = dict(os.environ, FMDJ_SHOTS=str(shots))
    r = subprocess.run([sys.executable, str(SRC / "web" / "make_site.py"), str(PKG), "test", str(site)],
                       cwd=SRC, env=env, capture_output=True, text=True)
    check(f"{case}: make_site runs", r.returncode == 0, r.stderr[-800:])
    return site


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

# ---- with shots: three tiny PNGs and their manifest
shots = OUT / "shots-in"
shutil.rmtree(shots, ignore_errors=True)
shots.mkdir(parents=True)
manifest = [{"name": n, "title": n.upper(), "file": f"{n}.png"} for n in ("song", "chain", "phrase")]
for i, m in enumerate(manifest):
    (shots / m["file"]).write_bytes(png(4, 3, (0x9B - i * 20, 0xBC, 0x0F)))
(shots / "manifest.json").write_text(json.dumps(manifest))
site = build("with", shots)
html, p = common("with", site)
for m in manifest:
    check(f"with: the page names shots/{m['file']}", f"shots/{m['file']}" in html)
    check(f"with: shots/{m['file']} is the one given",
          (site / "shots" / m["file"]).is_file() and
          (site / "shots" / m["file"]).read_bytes() == (shots / m["file"]).read_bytes())
    check(f"with: the page names the screen {m['title']}", m["title"] in p.text)
check("with: shots/manifest.json", (site / "shots" / "manifest.json").is_file()
      and json.loads((site / "shots" / "manifest.json").read_text()) == manifest)

# ---- without shots: an empty directory
empty = OUT / "shots-none"
shutil.rmtree(empty, ignore_errors=True)
empty.mkdir(parents=True)
site = build("without", empty)
html, p = common("without", site)
check("without: no shots/ reference on the page", "shots/" not in html)
check("without: no shots/ directory", not (site / "shots").exists())

# ---- a site built over one with shots loses them (no stale pictures)
site = OUT / "with"
r = subprocess.run([sys.executable, str(SRC / "web" / "make_site.py"), str(PKG), "test", str(site)], cwd=SRC,
                   env=dict(os.environ, FMDJ_SHOTS=str(empty)), capture_output=True, text=True)
check("rebuilt without: the old shots are gone", r.returncode == 0 and not (site / "shots").exists()
      and "shots/" not in (site / "index.html").read_text(encoding="utf-8"))

# ---- shots are not copied into an output that lies inside another git repository
foreign = OUT / "foreign-repo"
shutil.rmtree(foreign, ignore_errors=True)
foreign.mkdir(parents=True)
subprocess.run(["git", "init", "-q", str(foreign)], check=True)
site = foreign / "site"
r = subprocess.run([sys.executable, str(SRC / "web" / "make_site.py"), str(PKG), "test", str(site)], cwd=SRC,
                   env=dict(os.environ, FMDJ_SHOTS=str(shots)), capture_output=True, text=True)
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
