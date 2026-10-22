#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# LSDj fork: 2026 DEADACTIVE
# FMDj's site onto GitHub Pages: the gh-pages branch of origin (DeadActive/FMDj), built here (no GitHub Actions).
#   tools/fmdj_publish.sh VERSION            e.g. 2026.10.22
# Needs: the firmware without a ROM (FELUCCA_LSDJ=1 ./build.sh: build/lsdj-base.fwsc), its WebAssembly
# (web/lsdj_wasm.sh, web/sim/build.sh), and the screenshots (tools/fmdj_shots.py: build/fmdj/shots,
# build/fmdj/dmgo-shots; published as the owner chose, 2026-10-22). Every firmware package on the site must be
# lsdj-base.fwsc byte for byte: the private one (your ROM in it) never goes out, nor any ROM or save.
set -e
cd "$(dirname "$0")/.."
VER=${1:?usage: tools/fmdj_publish.sh VERSION}
BASE=build/lsdj-base.fwsc
OUT=build/sites/public
PAGES=build/gh-pages
[ -f "$BASE" ] || { echo "fmdj_publish: no $BASE (FELUCCA_LSDJ=1 ./build.sh)"; exit 1; }
rm -rf "$OUT"
FMDJ_SHOTS=build/fmdj/shots FMDJ_DMGO_SHOTS=build/fmdj/dmgo-shots python3 web/make_site.py "$BASE" "fmdj-$VER" "$OUT"
python3 - "$OUT" "$BASE" <<'EOF'
import sys
from pathlib import Path
out, base = Path(sys.argv[1]), Path(sys.argv[2]).read_bytes()
bad = [p for p in out.rglob("*") if p.suffix.lower() in (".gb", ".gbc", ".sav", ".lsdprj", ".lsdsng")]
pk = list(out.rglob("*.fwsc"))
bad += [p for p in pk if p.read_bytes() != base]
if bad or not pk:
    sys.exit("fmdj_publish: not publishing: " + (", ".join(map(str, bad)) or "no firmware package on the site"))
print(f"fmdj_publish: {len(pk)} firmware packages, all without a ROM; no ROM or save on the site")
EOF
if [ ! -d "$PAGES" ]; then
    git fetch origin gh-pages 2>/dev/null || true
    if git rev-parse -q --verify origin/gh-pages >/dev/null; then
        git worktree add -B gh-pages "$PAGES" origin/gh-pages
    else
        git worktree add --orphan -b gh-pages "$PAGES"
    fi
fi
rsync -a --delete --exclude .git "$OUT"/ "$PAGES"/
touch "$PAGES/.nojekyll"
git -C "$PAGES" add -A
if git -C "$PAGES" diff --cached --quiet; then
    echo "fmdj_publish: the site is as published"
else
    git -C "$PAGES" commit -q -m "site: FMDj $VER"
fi
git -C "$PAGES" push origin gh-pages
echo "fmdj_publish: https://deadactive.github.io/FMDj/"
