# FMDj landing page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A landing page for the project, now named FMDj, with real LSDj screenshots on top; the new name everywhere.

**Architecture:** A Python tool makes the screenshots with the existing host emulator (`tests/lsdj/lsdj_host.c`) into
git-ignored `build/fmdj/shots/`. The landing page (`web/fmdj/`) draws the FM-1 with the simulator's own panel code
(extracted from `web/sim/app.js` into `web/sim/panel.js`) and shows the screenshots in its screen. `web/make_site.py`
makes it the site's root page. Then the name changes on every page, the docs and the firmware's start screen.

**Tech Stack:** Python 3 (stdlib only), plain ES modules and CSS (no libraries, no CDN), C host emulator (`cc`),
the firmware build (`./build.sh`, Docker).

**Spec:** `docs/superpowers/specs/2026-10-17-fmdj-landing-design.md`

## Global Constraints

- Repository and branch: `~/Desktop/dev/fm1-lsdj-xfer`, branch `fmdj`. Commit there; never push; never merge.
- Never edit anything under `~/Desktop/dev/fm1-drummachine/` (another project).
- Nothing made from the LSDj ROM is committed: the ROM, saves, screenshots and private packages stay in git-ignored
  `build/` (tools refuse an output path that is not git-ignored: `git check-ignore -q PATH`).
- The owner's ROM is `$LSDJ_ROM` (on this Mac: `~/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb`); tests that need it are
  skipped (print "skipped", exit 0) when `LSDJ_ROM` is unset. The owner's save for the screenshots:
  `$FMDJ_SAVE` (on this Mac: `~/Downloads/dontstop/DONTSTOP_v682-v901.sav`).
- The project's name is **FMDj** (in capitals on the device: `FMDJ`). Where text means LSDj itself (the program, the
  ROM, e.g. `NO LSDJ ROM`, "your own LSDj ROM") it stays LSDj.
- The fork is credited as DEADACTIVE (file headers: `SPDX-License-Identifier: GPL-3.0-only` and `LSDj fork: 2026
  DEADACTIVE`, as the neighbouring files).
- Firmware: the only firmware change allowed is the start-screen text in `firmware/src/core_main.c`. The build must
  print `ok    update path: unchanged`; if it does not, that change is undone.
- Page copy: short plain English sentences, the tone of the existing pages (`web/sim/index.html`).
- The whole host suite passes: `LSDJ_ROM=~/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb sh tests/run_tests.sh` ends with
  `ALL HOST TESTS PASSED` (it needs one `FELUCCA_LSDJ=1 LSDJ_ROM=... ./build.sh` done before; the build tree is there).
- Each new test is added to `tests/run_tests.sh` next to the other LSDj tests.

## Review Focus

- A screenshot run that silently produces a blank or wrong screen (the walk did not reach it): each PNG must differ
  from the others, and the tool must fail loudly if LSDj reports errors.
- The landing page with no screenshots (a site built without the ROM): no broken image references, no empty gallery.
- The simulator page after `panel.js` is extracted: it must behave exactly as before (its node test, and it still
  builds its panel).
- The rename must not touch LSDj's own name where it means the program or its ROM.
- Any local link on the landing page that points at a file the site does not have.

---

### Task 1: The screenshot tool

**Files:**
- Create: `tools/fmdj_shots.py`
- Create: `tests/lsdj/fmdj_shots_test.py`
- Modify: `tests/run_tests.sh` (one `run` line after the other LSDj lines)

**Interfaces:**
- Consumes: `tests/lsdj/lsdj_host.c` (existing): `lsdj_host ROM FRAMES -s SAVE -k FROM:TO:KEYS ... -p SHOT.pgm`;
  key letters `l r u d a b s t` (left right up down A B SELECT START); `-k FROM:TO:KEYS` holds the keys for frames
  `FROM <= f < TO`; several `-k` may overlap. `-p` writes the last frame as binary PGM `P5 160 144 255`, each pixel
  `255 - shade * 85`, shade 0 (light) .. 3 (dark). Exit code 0 means the ROM ran without emulator errors and drew
  something. Build it as `tools/lsdj_rom.py` does: `cc -O2 -w -I firmware/src -I firmware/src/lsdj -o OUT tests/lsdj/lsdj_host.c`.
- Produces: `fmdj_shots.py ROM SAVE OUTDIR` writes `OUTDIR/<name>.png` for each screen and `OUTDIR/manifest.json` =
  `[{"name": "song", "title": "SONG", "file": "song.png"}, ...]` in the order SONG, CHAIN, PHRASE, INSTR, TABLE,
  SYNTH, WAVE, GROOVE, PROJECT. Functions importable for tests: `png(shades, scale=3) -> bytes` (shades: 160*144
  values 0..3), `keys_for(path, start=120) -> list[str]` (the `-k` arguments), `PATHS` (dict name -> list of
  directions), `PALETTE` (4 RGB tuples, light to dark).

LSDj's screen map (LSDj 9.4.2, measured on the host): the middle row SONG, CHAIN, PHRASE, INSTR, TABLE is walked with
SELECT + RIGHT from SONG; SELECT + UP from PHRASE is SYNTH, from TABLE is WAVE, from SONG is PROJECT; SELECT + DOWN
from SONG is GROOVE. LSDj takes SELECT + a direction only if SELECT is held two frames before the direction (the
PROJECT screen needs this).

- [ ] **Step 1: Write the failing test** `tests/lsdj/fmdj_shots_test.py` (plain script, prints `ok`/`FAILED` lines and
  `fmdj_shots_test: all passed` or `N FAILED`, exit code = failures != 0; import the tool with
  `sys.path.insert(0, tools)`):
  1. `PATHS` has exactly the 9 names above with: song `[]`, chain `["r"]`, phrase `["r","r"]`, instr `["r","r","r"]`,
     table `["r","r","r","r"]`, synth `["r","r","u"]`, wave `["r","r","r","r","u"]`, groove `["d"]`, project `["u"]`.
  2. `keys_for(["r","u"], start=120)` equals exactly
     `["-k", "100:104:t", "-k", "120:125:s", "-k", "122:123:r", "-k", "140:145:s", "-k", "142:143:u"]`
     (START held at frames 100..103 starts the song; then each step every 20 frames from `start`: SELECT held 5
     frames, the direction in its third frame).
  3. `png()` of a 160x144 image whose shade is `(x // 40) % 4` decodes (with `zlib` in the test: PNG signature,
     IHDR width 480 height 432 bit depth 8 colour type 2, IDAT rows each with filter byte 0) to pixels where pixel
     (x, y) equals `PALETTE[(x // 3 // 40) % 4]`; `PALETTE` = `[(0x9B,0xBC,0x0F),(0x8B,0xAC,0x0F),(0x30,0x62,0x30),(0x0F,0x38,0x0F)]`.
  4. Running `tools/fmdj_shots.py ROM SAVE /tmp/x` (an output not git-ignored) exits non-zero with a message
     containing `git-ignored`, and writes nothing.
  5. With `LSDJ_ROM` set and `build/lsdj/demo.sav` present (else print a skip line): run the tool on them into
     `build/tests/fmdj_shots/`; expect 9 PNGs named as in the manifest, `manifest.json` with 9 entries in order, and
     all 9 PNG files' bytes distinct from each other.
- [ ] **Step 2: Run it**: `python3 tests/lsdj/fmdj_shots_test.py` — expect a failure (no tool).
- [ ] **Step 3: Write `tools/fmdj_shots.py`** (stdlib only): the checks of `tools/lsdj_rom.py` for the output path
  (`git -C <repo> check-ignore -q OUTDIR`, refuse with a message containing `git-ignored`); build `lsdj_host` into
  `build/host/lsdj_host`; for each screen run `lsdj_host ROM <frames> -s SAVE <keys_for(path)> -p <tmp>.pgm` with
  `<frames>` = 120 + 20 * len(path) + 60, fail (non-zero exit, the host's output in the message) if the host exits
  non-zero; read the PGM, turn grey values into shades (`(255 - v) // 85`), write the PNG (3x nearest, colour type 2,
  `zlib.compress`, CRCs with `zlib.crc32`) and the manifest. Print one line per screen.
- [ ] **Step 4: Run the test** — all passed (part 5 skipped without the ROM; run it once with
  `LSDJ_ROM=~/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb` too, and look at one PNG: it must show the screen named).
- [ ] **Step 5: Add to `tests/run_tests.sh`**: `run "FMDj screenshots: the walk to each LSDj screen, the PNGs (the run skipped without LSDJ_ROM)" python3 tests/lsdj/fmdj_shots_test.py`
- [ ] **Step 6: Commit** `tools/fmdj_shots.py tests/lsdj/fmdj_shots_test.py tests/run_tests.sh` with a message
  starting `fmdj: the screenshot tool`.

### Task 2: The landing page

**Files:**
- Create: `web/sim/panel.js` (the panel building and placing, moved out of `web/sim/app.js`)
- Modify: `web/sim/app.js` (imports `panel.js`; no behaviour change)
- Create: `web/fmdj/index.html`, `web/fmdj/fmdj.css`, `web/fmdj/fmdj.js`
- Modify: `web/make_site.py` (the root page)
- Create: `tests/lsdj/fmdj_site_test.py`
- Modify: `tests/run_tests.sh`

**Interfaces:**
- Consumes: Task 1's `manifest.json` format; the site layout `web/make_site.py` writes today:
  `webapp/lsdj-install/` (installer with your ROM), `webapp/lsdj/` (song page), `webapp/lsdj-sim/` (simulator, its
  modules served as files: `controls.js`, `layout.js`, `sim.css`, ...), `webapp/installer/` (plain installer).
  The simulator's panel code is in `web/sim/app.js` (`build()`, `relayout()`, `place()`, `span()`, `addHint()`, the
  green palette `RAMP`/`green()`/`LUT`/`SHADE`), using `web/sim/layout.js` and `web/sim/controls.js`.
- Produces: `web/sim/panel.js` exporting `buildPanel(device, lcd)` (the elements, as `build()` does, returning the
  `els` object), `layoutPanel(device, fit, els, lcd, availWidth)` (as `relayout()`, returning `phone`), and the
  palette helpers `green(l)`, `SHADE`. The site's root `index.html` is the landing page when the package is an LSDj
  one (`lsdj-base.fwsc` / `lsdj-private.fwsc`); screenshots copied from `build/fmdj/shots/` (if `manifest.json`
  exists there) to the site's `shots/`.

- [ ] **Step 1: Write the failing test** `tests/lsdj/fmdj_site_test.py`: build a site with
  `python3 web/make_site.py build/lsdj-private.fwsc test build/tests/fmdj_site/<case>` (skip with a message if
  `build/lsdj-private.fwsc` or `build/web/sim/lsdjsim.wasm` is missing), for two cases:
  - **with shots**: before building, put a fake shots dir in place by setting the env var `FMDJ_SHOTS` to a
    directory with three tiny valid PNGs and a `manifest.json` listing them (make_site reads `FMDJ_SHOTS`, default
    `build/fmdj/shots`);
  - **without shots**: `FMDJ_SHOTS` pointing at an empty directory.
  Check, in each: the root `index.html` has `<title>` containing `FMDj`, a heading with `FMDj`, links to
  `webapp/lsdj-install/`, `webapp/lsdj/`, `webapp/lsdj-sim/`; every local `href`/`src` in the root page resolves to a
  file or directory of the built site; with shots, the page names each manifest file and `shots/` holds them; without
  shots, the page references no `shots/` file. Also: `node tests/lsdj/lsdj_sim_test.mjs` still passes (run it).
- [ ] **Step 2: Run it** — expect failures.
- [ ] **Step 3: Move the panel code** from `web/sim/app.js` into `web/sim/panel.js` (same code, exported), and make
  `app.js` import and use it. Run `node tests/lsdj/lsdj_sim_test.mjs` and load-check `node --check web/sim/app.js`.
- [ ] **Step 4: The page** `web/fmdj/index.html` + `fmdj.css` + `fmdj.js`, the sections of the spec in order.
  `fmdj.js` imports `../webapp/lsdj-sim/panel.js` etc. as served on the site (the page lives at the site root:
  paths are `webapp/lsdj-sim/panel.js`), builds the panel (inert: no audio, no input), and every 3 s draws the next
  screenshot into the LCD canvas (the 160x144 picture at 1.5x in rows 4..219, as the firmware shows it), with the
  gallery of all screenshots under the device (clicking one shows it). It reads `shots/manifest.json`; if that is
  missing, no gallery and the screen stays the empty green. Style: the simulator's look (`sim.css` device styles via
  `webapp/lsdj-sim/sim.css`; page styles in `fmdj.css`).
- [ ] **Step 5: `web/make_site.py`**: for an LSDj package write the root `index.html` from `web/fmdj/index.html`,
  copy `fmdj.css`, `fmdj.js`, `panel.js` (into `webapp/lsdj-sim/`), and the shots (from `FMDJ_SHOTS` or
  `build/fmdj/shots`) to `shots/` when its `manifest.json` exists; otherwise keep today's redirect.
- [ ] **Step 6: Run the test** — all passed; add it to `tests/run_tests.sh`; commit with a message starting
  `fmdj: the landing page`.

### Task 3: The name FMDj everywhere

**Files:**
- Modify: `web/lsdj.html`, `web/lsdj_install.html`, `web/sim/index.html`, `web/sim/app.js` (or `web/sim/panel.js`,
  wherever the bezel print `LSDJ · FM-1` is), `docs/lsdj/*.md`, `docs/lsdj/SIMULATOR.md`, `firmware/src/core_main.c`
- Create: `tests/lsdj/fmdj_name_test.py`
- Modify: `tests/run_tests.sh`

- [ ] **Step 1: Write the failing test** `tests/lsdj/fmdj_name_test.py`: the files above (the pages and the docs)
  contain `FMDj` (or `FMDJ` for the bezel print) and none contains the phrase `LSDj on the FM-1`; the page titles
  start with `FMDj`; `firmware/src/core_main.c` draws `"FMDJ"` on its start screen; and the program's own name is
  left where it means LSDj: `web/sim/index.html` still says `LSDj is by Johan Kotlinski`, and
  `firmware/src/lsdj/lsdj.c` still has `NO LSDJ ROM`.
- [ ] **Step 2: Run it** — expect failures.
- [ ] **Step 3: Rename** in those files (titles, headings, the bezel print `FMDJ · FM-1`, doc titles and the
  sentences that name the project; not where LSDj means the program or the ROM). In `core_main.c` change only the
  string `"LSDJ"` of the start screen to `"FMDJ"`.
- [ ] **Step 4: Firmware build**: `PYTHON=$HOME/Desktop/dev/fm1-lsdj/build/venv/bin/python FELUCCA_LSDJ=1 LSDJ_ROM=~/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb ./build.sh`
  must end with exit 0 and print `ok    update path: unchanged`. If the gate fails, put `"LSDJ"` back, drop that check
  from the test (say why in a comment), and report it.
- [ ] **Step 5: Run the test and the whole suite** (`sh tests/run_tests.sh` with `LSDJ_ROM` set: `ALL HOST TESTS
  PASSED`); add the test to `tests/run_tests.sh`; commit with a message starting `fmdj: the name`.
