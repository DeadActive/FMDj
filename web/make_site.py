#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Make the site (GitHub Pages):

  index.html                  redirect to the installer (the old URL keeps working)
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
import json
import re
import shutil
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
