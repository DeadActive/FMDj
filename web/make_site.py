#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Make the site (GitHub Pages):

  index.html                  redirect to the installer (the old URL keeps working); for an LSDj package the
                              FMDj landing page (web/fmdj/: + fmdj.css, fmdj.js, and the screens of the programs
                              made by tools/fmdj_shots.py: shots/lsdj/ LSDj's from FMDJ_SHOTS or build/fmdj/shots,
                              shots/dmgo/ DMGo's from FMDJ_DMGO_SHOTS or build/fmdj/dmgo-shots, each when its
                              manifest.json is there; into a git-ignored OUT_DIR only, not inside another git
                              repository: they are made from your ROMs)
  firmware/felucca-VER.fwsc   the package (+ LICENSE, LICENSING.md, LICENSES/: the package holds
                              JieLi SDK files under Apache-2.0, see LICENSING.md)
  webapp/installer/index.html index_pkg.html, self-contained (fm1pkg.js, fm1ota.js, metadata inlined)
  webapp/editor/index.html    editor.html (+ fukiai.ttf, FUKIAI-LICENSE.txt)
  webapp/lsdj/index.html      lsdj.html, self-contained (lsdjfs.js, lsdjlink.js inlined): the LSDj build's songs
  src/                        not touched

  web/make_site.py build/felucca-X.Y.fwsc X.Y OUT_DIR

The package must be one made by tools/fm1pkg_make.py (Felucca's own loader, no vendor files).
Its identity (FM-1_9xx) is read from the package; the device must report it after
the install.
"""
import html as H
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (fm1pkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


SHOTS = (("lsdj", "LSDj", "FMDJ_SHOTS", "shots"), ("dmgo", "DMGo", "FMDJ_DMGO_SHOTS", "dmgo-shots"))
SHOTS_NOTE = {"lsdj": "From a song, on the FM-1's emulator.",
              "dmgo": "From an empty start: CH1's steps set with the white keys, CH3's and CH4's from DMGo's presets."}


def landing_shots(out):
    """The programs' screens (tools/fmdj_shots.py: manifest.json + PNGs; LSDj's in FMDJ_SHOTS or build/fmdj/shots,
    DMGo's in FMDJ_DMGO_SHOTS or build/fmdj/dmgo-shots) -> copied to OUT/shots/lsdj/ and OUT/shots/dmgo/, and the
    galleries' markup for the landing page, one under each program's name; "" (no gallery) without any. They are
    made from the owner's ROMs: only into an OUT_DIR git ignores (the local site), never one that could be
    published."""
    found = []
    for prog, who, var, sub in SHOTS:
        src = Path(os.environ.get(var) or HERE.parent / "build" / "fmdj" / sub)
        if (src / "manifest.json").is_file():
            found.append((prog, who, src))
    if not found:
        return ""
    where = ", ".join(str(src) for _, _, src in found)
    if subprocess.run(["git", "-C", str(HERE.parent), "check-ignore", "-q", str(out.resolve())],
                      capture_output=True).returncode != 0:
        print(f"site: {out} is not git-ignored here: the programs' screens ({where}) left out, they are made from "
              f"your ROMs")
        return ""
    d = out.resolve()
    while not d.exists():
        d = d.parent
    top = subprocess.run(["git", "-C", str(d), "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if top.returncode == 0 and Path(top.stdout.strip()).resolve() != HERE.parent.resolve():
        print(f"site: {out} is inside another git repository ({top.stdout.strip()}): the programs' screens "
              f"({where}) left out, they are made from your ROMs")
        return ""
    groups = []
    for prog, who, src in found:
        shots = json.loads((src / "manifest.json").read_text(encoding="utf-8"))
        if not isinstance(shots, list) or any(not isinstance(s, dict) or not isinstance(s.get("title"), str)
                                              or not re.fullmatch(r"[A-Za-z0-9_-]+\.png", str(s.get("file")))
                                              or not (src / s["file"]).is_file() for s in shots):
            raise SystemExit(f"{src}/manifest.json: not a list of screens with their PNG files (tools/fmdj_shots.py)")
        other = sorted({str(s.get("prog")) for s in shots if s.get("prog", prog) != prog})
        if other:
            raise SystemExit(f"{src}/manifest.json: screens of {', '.join(other)}, not {who}'s: make {who}'s with "
                             f"tools/fmdj_shots.py into it")
        if shots:
            groups.append((prog, who, src, shots))
    html = ""
    for prog, who, src, shots in groups:
        (out / "shots" / prog).mkdir(parents=True)
        for s in shots:
            shutil.copy(src / s["file"], out / "shots" / prog / s["file"])
        shutil.copy(src / "manifest.json", out / "shots" / prog / "manifest.json")
        name = "DMGo!" if prog == "dmgo" else who
        items = "".join(
            f'\n        <li><button type="button" data-prog="{who}" data-title="{H.escape(s["title"])}" '
            f'aria-pressed="false"><img src="shots/{prog}/{s["file"]}" alt="{who}\'s {H.escape(s["title"])} screen" '
            f'width="480" height="432"></button>{H.escape(s["title"])}</li>' for s in shots)
        html += (f'    <div class="prog">\n      <h3 id="shots-{prog}">{name}</h3>\n'
                 f'      <ul class="gallery" aria-labelledby="shots-{prog}">{items}\n      </ul>\n'
                 f'      <p class="small">{SHOTS_NOTE[prog]}</p>\n    </div>\n')
    if not html:
        return ""
    return ('  <section id="shots" class="shots" aria-label="Screens">\n' + html +
            '    <p class="small">The FM-1 above shows each in turn. Click one to keep it there.</p>\n  </section>')


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_9\d\d", product):
        raise SystemExit(f"{pkg}: identity {product!r} is not a Felucca package (FM-1_9xx)")
    if b"FELUCCA-LOADER-1" not in raw:              # Felucca loader marker: never publish a package with vendor files
        raise SystemExit(f"{pkg}: no Felucca loader in it; the site ships only fm1pkg_make.py packages "
                         "(a package patched from an official one carries vendor files)")
    html = (HERE / "index_pkg.html").read_text(encoding="utf-8")
    lib = strip_module((HERE / "fm1pkg.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((HERE / "fm1ota.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((HERE / "fm1backup.js").read_text(encoding="utf-8"))
    name = f"felucca-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
    meta = json.dumps({"version": version, "product": product, "pkg": "../../firmware/" + name})
    for mark in ("/*LIB*/", "/*META*/"):
        if html.count(mark) != 1:
            raise SystemExit(f"index_pkg.html must contain {mark} once; update make_site.py")
    html = html.replace("/*LIB*/", lib).replace("/*META*/", meta)
    inst, ed, fw = out / "webapp" / "installer", out / "webapp" / "editor", out / "firmware"
    for d in (inst, ed, fw):
        d.mkdir(parents=True, exist_ok=True)
    for old in fw.glob("felucca-*.fwsc"):          # one package: the current one
        old.unlink()
    (inst / "index.html").write_text(html, encoding="utf-8")
    shutil.copy(pkg, fw / name)
    lic = HERE.parent / "LICENSES"                  # the package holds JieLi SDK files (Apache-2.0): their
    (fw / "LICENSES").mkdir(exist_ok=True)          # licence travels next to it, with Felucca's own
    names = sorted(f.name for f in lic.glob("*.txt"))
    for n in names:
        shutil.copy(lic / n, fw / "LICENSES" / n)
    (fw / "LICENSES" / "index.html").write_text(    # the installer links this folder: Pages lists no folders
        '<!doctype html><meta charset="utf-8"><title>Felucca licences</title><h1>Licence texts</h1><ul>'
        + "".join(f'<li><a href="{n}">{n}</a></li>' for n in names)
        + '</ul><p><a href="../LICENSING.md">LICENSING.md</a> · <a href="../LICENSE">LICENSE (GPL-3.0)</a></p>\n',
        encoding="utf-8")
    for doc in ("LICENSE", "LICENSING.md"):
        shutil.copy(HERE.parent / doc, fw / doc)
    shutil.copy(HERE / "editor.html", ed / "index.html")
    for f in ("fukiai.ttf", "FUKIAI-LICENSE.txt", "fm1backup.js"):
        if (HERE / f).exists():
            shutil.copy(HERE / f, ed / f)
    page = (HERE / "lsdj.html").read_text(encoding="utf-8")      # the LSDj build's song page (the USB console)
    if page.count("/*LIB*/") != 1:
        raise SystemExit("lsdj.html must contain /*LIB*/ once; update make_site.py")
    songs_lib = strip_module((HERE / "lsdjfs.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((HERE / "lsdjlink.js").read_text(encoding="utf-8"))
    (out / "webapp" / "lsdj").mkdir(parents=True, exist_ok=True)
    (out / "webapp" / "lsdj" / "index.html").write_text(page.replace("/*LIB*/", songs_lib), encoding="utf-8")
    first = "webapp/installer/"
    note = ""
    # the LSDj install page (lsdj_install.html): the user's own ROM goes into the firmware that holds none
    # (lsdj-base.fwsc, next to the package or the package itself), in the browser, with the emulator and the
    # block's maker as WebAssembly (web/lsdj_wasm.sh). Nothing of LSDj is on the site.
    # Only for an LSDj package (the firmware alone, or the owner's private one next to it), and only a base of the
    # same identity with our loader in it.
    base = pkg if pkg.name == "lsdj-base.fwsc" else pkg.with_name("lsdj-base.fwsc")
    wasm = HERE.parent / "build" / "web" / "lsdj_emu.wasm"
    for old in fw.glob("lsdj-base-*.fwsc"):
        old.unlink()
    shutil.rmtree(out / "webapp" / "lsdj-install", ignore_errors=True)
    if pkg.name in ("lsdj-base.fwsc", "lsdj-private.fwsc") and base.exists() and wasm.exists():
        braw = base.read_bytes()
        if product_of(braw) != product or b"FELUCCA-LOADER-1" not in braw:
            raise SystemExit(f"{base}: not this build's firmware without a ROM ({product_of(braw)!r})")
        sys.path.insert(0, str(HERE.parent / "tools"))
        import lsdj_demo
        args, end = lsdj_demo.key_args(60)
        keys = [[int(a), int(b), k] for a, b, k in (x.split(":") for x in args[1::2])]
        bname = f"lsdj-base-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
        ipage = (HERE / "lsdj_install.html").read_text(encoding="utf-8")
        for mark in ("/*LIB*/", "/*META*/"):
            if ipage.count(mark) != 1:
                raise SystemExit(f"lsdj_install.html must contain {mark} once; update make_site.py")
        ilib = "\n".join(strip_module((HERE / n).read_text(encoding="utf-8"))
                         for n in ("fm1pkg.js", "fm1ota.js", "lsdjinstall.js"))
        imeta = json.dumps({"version": version, "product": product_of(base.read_bytes()),
                            "pkg": "../../firmware/" + bname, "wasm": "lsdj_emu.wasm",
                            "demo": {"keys": keys, "frames": end + 60}})
        idir = out / "webapp" / "lsdj-install"
        idir.mkdir(parents=True, exist_ok=True)
        (idir / "index.html").write_text(ipage.replace("/*LIB*/", ilib).replace("/*META*/", imeta), encoding="utf-8")
        shutil.copy(wasm, idir / "lsdj_emu.wasm")
        shutil.copy(base, fw / bname)
        note = f", webapp/lsdj-install (firmware/{bname}, no ROM in it)"
        if base == pkg:                             # a site of the ROM-less firmware: its front door is that page
            first = "webapp/lsdj-install/"
    # the LSDj simulator (web/sim/): the LSDj unit as WebAssembly behind the FM-1's panel, the user's own ROM chosen
    # on the page. Its modules are served as they are; it shares the install page's code that makes the cartridge
    # block (lsdjinstall.js, fm1pkg.js, lsdj_emu.wasm, the demo's keys).
    simwasm = HERE.parent / "build" / "web" / "sim" / "lsdjsim.wasm"
    sdir = out / "webapp" / "lsdj-sim"
    shutil.rmtree(sdir, ignore_errors=True)
    if pkg.name in ("lsdj-base.fwsc", "lsdj-private.fwsc") and simwasm.exists() and wasm.exists():
        sys.path.insert(0, str(HERE.parent / "tools"))
        import lsdj_demo
        args, end = lsdj_demo.key_args(60)
        sdir.mkdir(parents=True)
        for n in ("index.html", "app.js", "sim.css", "engine.js", "worklet.js", "controls.js", "layout.js", "store.js",
                  "audio.js", "simlink.js", "songs.js", "panel.js"):
            shutil.copy(HERE / "sim" / n, sdir / n)
        for n in ("lsdjinstall.js", "fm1pkg.js", "lsdjlink.js", "lsdjfs.js"):
            shutil.copy(HERE / n, sdir / n)
        shutil.copy(simwasm, sdir / "lsdjsim.wasm")
        shutil.copy(wasm, sdir / "lsdj_emu.wasm")
        (sdir / "demo.json").write_text(json.dumps({
            "keys": [[int(a), int(b), k] for a, b, k in (x.split(":") for x in args[1::2])], "frames": end + 60}))
        note += ", webapp/lsdj-sim"
    # the FMDj landing page (web/fmdj/): the front door of an LSDj site with its install page and simulator (it links
    # both and takes the simulator's panel.js and sim.css); LSDj's screens on it when there are any
    shutil.rmtree(out / "shots", ignore_errors=True)
    for n in ("fmdj.css", "fmdj.js"):
        (out / n).unlink(missing_ok=True)
    if pkg.name in ("lsdj-base.fwsc", "lsdj-private.fwsc") and (out / "webapp" / "lsdj-install").is_dir() and sdir.is_dir():
        land = (HERE / "fmdj" / "index.html").read_text(encoding="utf-8")
        if land.count("<!--SHOTS-->") != 1:
            raise SystemExit("fmdj/index.html must contain <!--SHOTS--> once; update make_site.py")
        shots = landing_shots(out)
        (out / "index.html").write_text(land.replace("<!--SHOTS-->", shots), encoding="utf-8")
        for n in ("fmdj.css", "fmdj.js"):
            shutil.copy(HERE / "fmdj" / n, out / n)
        note += ", the FMDj landing page" + (f" ({len(list((out / 'shots').rglob('*.png')))} screens)" if shots else "")
    else:
        (out / "index.html").write_text(
            '<!doctype html><meta charset="utf-8"><title>Felucca</title>'
            f'<meta http-equiv="refresh" content="0; url={first}">'
            f'<a href="{first}">Felucca installer</a>\n', encoding="utf-8")
    print(f"site: {out}: webapp/installer ({len(html)} B), webapp/editor, firmware/{name} ({len(raw)} B, {product})"
          + note)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
