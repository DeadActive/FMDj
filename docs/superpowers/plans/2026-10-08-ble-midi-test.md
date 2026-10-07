# BLE MIDI Test Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a test firmware on the stripped Felucca core that scans for BLE devices, connects to a BLE-MIDI device
(M-VAVE SMC-Mixer), shows its MIDI on screen and forwards it to USB MIDI. The update path stays reachable in every
build.

**Architecture:**
- The unchanged `-Os` core (boot, USB, update entry, LCD, input) calls a self-contained `firmware/src/ble/` module
  from its main loop.
- The module runs JieLi's prebuilt BT controller and host libraries (LLVM bitcode, from the AC79 SDK) on a small
  cooperative OS layer of our own. Context switching is in `hal/fm1_ctx.S`.
- Pure-C parts are developed test-first on the host: the BLE-MIDI parser, the scan table, and the OS layer's
  queues, semaphores, timers and heap.
- Device steps A–E come in order. Each is a separate build that the user installs from the previous one.

**Tech stack:**
- C (freestanding, pi32v2, JieLi clang 4.0.1 via `./build.sh` in Docker) and pi32v2 assembly.
- Host tests in C (`cc`) and Python 3.
- JieLi AC79 SDK V1.2.0 libraries: `btctrler.a`, `btstack.a` and the radio-init members.

**Spec:** `docs/superpowers/specs/2026-10-08-ble-midi-test-design.md` (read it first).

## Global Constraints

- **The update path must always be reachable:**
  - The core (`usb.c`, `ota.c`, `ota_hw.c`, `libc.c`, `hal/fm1_flash.h`, `fm1_sys.h`, `fm1_time.h`, `fm1_usb.h`)
    is built at `-Os`, never `-O2`, never `FELUCCA_OPT`.
  - `build/loader/ota.bin` sha256 must be
    `e671adf0ce6e4f69ac8514087c8833b1969fa4c470645786f19968e90b9a630f`.
- **No Bluetooth code runs before the main loop.** BT starts only from the PLAY key handler (`ble_start()`).
- **The BT code never writes flash:** not VM `0x94000`, not BTIF `0xE9000`, not key_mac `0xFF000`, nothing at all.
- **Experiments are user-triggered:** each device step is a separate build, installed by the user through the web
  installer from the previous build. Claude never installs firmware and never runs `tools/fm1_install.py`, the web
  installer or M-UPGRADE.
- **Headers and credits:**
  - New files carry `SPDX-License-Identifier: GPL-3.0-only` and `LSDj fork` style credit `2026 DEADACTIVE`
    (`/* SPDX-License-Identifier: GPL-3.0-only\n * BLE MIDI test: 2026 DEADACTIVE */`).
  - JieLi SDK material is Apache-2.0 and keeps its notices.
- **Register access:** `src/` (top level and `src/ble/`) has no register literals, volatile casts or inline asm;
  hardware access goes through `hal/` (`tools/build.py` `mmio_check`). One exception: `RING_PUBLISH()`'s empty
  barrier.
- **Integer only:** no floating point on the target.
- **Units:** BLE scan interval and window in 0.625 ms units; connection interval in 1.25 ms units; supervision
  timeout in 10 ms units.
- **BLE-MIDI UUIDs:** service `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`, characteristic
  `7772E5DB-3868-4112-A1A9-F2669D106BF3`. In advertising data they appear little-endian:
  `00 C7 C4 4E E3 6C 51 A7 33 4B E8 ED 5A 0E B8 03`.
- **Build:** `PYTHON=<venv with Pillow>/bin/python ./build.sh` (the system python3 has no Pillow; the session venv is
  `/private/tmp/claude-501/-Users-evech-Desktop-dev-fm1-lsdj/<session>/scratchpad/venv`, or create one with
  `python3 -m venv ~/fm1-venv && ~/fm1-venv/bin/pip install Pillow fonttools`). Docker must run; retry on
  "exec format error".

## Review Focus

- **A BLE-MIDI packet with running status and no timestamp before the second message** (`80 80 90 3C 64 3E 64`):
  both note-ons are delivered. Pinned in Task 3.
- **A SysEx split across two notifications**, the second starting with plain data bytes (no timestamp): delivered as
  one SysEx with the right length, and the next note after it still parses. Pinned in Task 3.
- **The scan table full (16) when a stronger new device appears:** the weakest is replaced, and a device seen again
  only updates (no duplicate). A scan response that only carries the name merges into the earlier advertising entry
  by address. Pinned in Task 4.
- **A BT task that never blocks or an IRQ storm:** `ble_service()` still returns within its 50 ms budget, so the
  watchdog and the update entry keep running. Pinned in Task 6 (budget test).
- **A disconnect in any state** (connecting, discovering, subscribed): the state returns to the list, counters stay
  readable, and PLAY can rescan. Pinned in Task 8 (state machine test).

---

## File structure

| File | Responsibility |
| --- | --- |
| `firmware/src/felucca.c` | Unity root of the stripped core; includes `ble/ble.c` when `FELUCCA_BLE` |
| `firmware/src/core_main.c`, `core_audio.c`, `console.c` | The stripped core (from `lsdj` `407b0e7` without probe code); main loop gains the BLE hooks |
| `firmware/src/ble/ble.c` | Unity include list of the module (keeps `felucca.c`'s change to one line) |
| `firmware/src/ble/ble_midi.c` / `.h` | BLE-MIDI packet parser → USB-MIDI event packets (pure C) |
| `firmware/src/ble/ble_scan.c` / `.h` | Scan results table and AD parsing (pure C) |
| `firmware/src/ble/ble_os.c` / `.h` | Cooperative tasks, task queues, semaphores, mutexes, delays, timers, heap (pure C plus the `ctx` back end) |
| `firmware/src/ble/ble_port.c` | What the JieLi libraries call besides the OS: IRQs, config reads, clock and low-power stubs, `config_*` constants, unused-feature stubs |
| `firmware/src/ble/ble_central.c` | The app on the SDK `ble_op_*` API: start, scan, connect, discover, subscribe, notifications |
| `firmware/src/ble/ble_ui.c` | Scan and connected screens, key handling |
| `firmware/src/ble/ble_sdk_abi.h` | Generated in Task 5: SDK object sizes and constants checked against the SDK headers |
| `firmware/hal/fm1_ctx.S`, `fm1_ctx.h` | Context switch for pi32v2 |
| `tools/build.py` | Loader pin; `FELUCCA_BLE` / `FELUCCA_BLE_STEP` flags; links the pinned SDK libraries |
| `tools/check_update_path.py` | Gate 2: update-path machine code equals the core reference |
| `tools/check_ble_boot.py` | Gate 4: no BT init reachable from boot outside the key handler |
| `tools/ble_libs.py` | Finds and pins the SDK libraries, writes `ble_sdk_abi.h` |
| `tests/ble/` | Host tests: `ble_midi_test.c`, `ble_scan_test.c`, `ble_os_test.c`, `ble_central_test.c`, `update_path.ref` |
| `tests/run_tests.sh` | Runs the new host tests |

---

### Task 1: The stripped core on branch `ble`

**Files:**
- Create: `firmware/src/core_main.c`, `firmware/src/core_audio.c` (from `lsdj` commit `407b0e7`)
- Modify: `firmware/src/felucca.c`, `firmware/src/console.c` (from `407b0e7`, probe code removed)

**Interfaces:**
- Produces: a core build identical in behaviour to the probe-1 core (−probe). Its `build/felucca.dis` is the
  reference for gate 2 (Task 2).

- [ ] **Step 1: Bring the core files over from the parked commit**

```bash
cd /Users/evech/Desktop/dev/fm1-lsdj
git switch ble
git checkout 407b0e7 -- firmware/src/core_main.c firmware/src/core_audio.c firmware/src/felucca.c firmware/src/console.c
```

- [ ] **Step 2: Remove the probe code**

In `firmware/src/felucca.c` delete exactly:

```c
#ifndef FELUCCA_PROBE
#define FELUCCA_PROBE 0          /* throwaway speed probe at boot (probe/probe.c) */
#endif
```

and

```c
#if FELUCCA_PROBE
#include "probe/probe.c"
#endif
```

In `firmware/src/core_main.c` delete the block that starts with `#if FELUCCA_PROBE` before `for (;;) {` (the two
`draw_text_box` lines for PLAY / REC) and the block `#if FELUCCA_PROBE` … `#endif` inside the loop (the PLAY / REC
edge handler). Nothing else changes.

In `firmware/src/console.c` delete the forward declarations block:

```c
#if FELUCCA_PROBE
static void probe_run(void);                         /* probe/probe.c (included after this file) */
static void probe_show(void);
static void probe_clk(void);
#endif
```

and the `#if FELUCCA_PROBE` … `#endif` command block (the `probe` and `clk` commands) in `con_exec`.

Check: `grep -rn "PROBE\|probe" firmware/src/*.c` prints nothing.

Then keep the boot path comparable for gate 2: `fm1_main` is `static` and called once, so the compiler inlines it
into `fm1_cstart`, and every later main-loop change would look like a changed boot path. In `core_main.c` change

```c
static void fm1_main(void)
```

to

```c
static __attribute__((noinline)) void fm1_main(void)    /* (gate 2 compares fm1_cstart, the boot path, alone) */
```

- [ ] **Step 3: Build and check the core**

Run: `PYTHON=$VENV/bin/python ./build.sh 2>&1 | tail -6`

Expected:
- `ok    image 663xx B` (about 66 KB; the probe-free core was 66,320 B);
- `loader: ota.bin 6693 B`;
- then:

```bash
shasum -a 256 build/loader/ota.bin
# e671adf0ce6e4f69ac8514087c8833b1969fa4c470645786f19968e90b9a630f
cp build/felucca.bin build/core_ref.bin    # Task 6 checks that the default build stays byte-identical to this
```

- [ ] **Step 4: Commit**

```bash
git add firmware/src/core_main.c firmware/src/core_audio.c firmware/src/felucca.c firmware/src/console.c
git commit -m "core: the stripped Felucca core (boot, USB, update entry, LCD, input; no synth)"
```

---

### Task 2: Build gates 1 and 2 (loader pin, update path unchanged)

**Files:**
- Modify: `tools/build.py` (after the loader is built, around `loader: ota.bin`)
- Create: `tools/check_update_path.py`, `tests/ble/update_path.ref`
- Modify: `tests/run_tests.sh`

**Interfaces:**
- Consumes: `build/felucca.dis` (objdump text: function label lines `name:`; instruction lines
  `  2005cfe:    00 49    \tinstruction text`).
- Produces:
  - `python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref` → exit 0 or 1;
  - `--write` regenerates the ref;
  - `--selftest` exits 0 if a mutated listing is caught.

- [ ] **Step 1: Add the loader pin to `tools/build.py`**

After the line that prints `loader: ota.bin ... B` (in `build_loader()`), add:

```python
    LOADER_SHA256 = "e671adf0ce6e4f69ac8514087c8833b1969fa4c470645786f19968e90b9a630f"
    got = hashlib.sha256((LDR / "ota.bin").read_bytes()).hexdigest()
    if got != LOADER_SHA256:          # the update loader must be byte-identical, whatever else changes
        raise SystemExit(f"build: loader ota.bin sha256 {got} is not the pinned {LOADER_SHA256}")
```

(`hashlib` is already imported by `build.py`; if not, add `import hashlib` at the top.)

- [ ] **Step 2: Write `tools/check_update_path.py`**

```python
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
TARGET = re.compile(r"-?\d+ <([^:>+]+)(?:\+0x[0-9a-f]+)?\s*:\s*[0-9a-f]+\s*>")


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
            out[name].append(TARGET.sub(r"<\1>", m.group(1)).strip())
    return out


def selected(funcs):
    return {n: body for n, body in funcs.items() if any(re.search(p, n) for p in PATTERNS)}


def render(funcs):
    return "".join(f"{n}:\n" + "".join(f"  {i}\n" for i in funcs[n]) for n in sorted(funcs))


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
    print("check_update_path: selftest ok")


def main(argv):
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
```

- [ ] **Step 3: Run the selftest**

Run: `python3 tools/check_update_path.py --selftest`
Expected: `check_update_path: selftest ok`

- [ ] **Step 4: Write the reference from the core build and check it against itself**

```bash
PYTHON=$VENV/bin/python ./build.sh >/dev/null
mkdir -p tests/ble
python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref --write
python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref
```

Expected: `update path: unchanged`. Also `grep -c ":$" tests/ble/update_path.ref` prints a count above 20 (the
USB, OTA and flash functions are there).

- [ ] **Step 5: Hook both gates into `tests/run_tests.sh`**

Add next to the other `run "..."` lines:

```sh
run "update path = the stripped core's (gate 2)" python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref
run "update path check self-test" python3 tools/check_update_path.py --selftest
```

- [ ] **Step 6: Commit**

```bash
git add tools/build.py tools/check_update_path.py tests/ble/update_path.ref tests/run_tests.sh
git commit -m "gates: pinned loader hash in build.py; update-path machine code checked against the core reference"
```

---

### Task 3: BLE-MIDI parser (host-tested)

**Files:**
- Create: `firmware/src/ble/ble_midi.h`, `firmware/src/ble/ble_midi.c`, `tests/ble/ble_midi_test.c`
- Modify: `tests/run_tests.sh`

**Interfaces:**
- Produces:

```c
typedef void (*ble_midi_out_fn)(void *ctx, uint32_t usb_pkt);   /* USB-MIDI event packet, cable 0 */
typedef struct {
    uint8_t status, need, got, d0, running;   /* message in progress; running status (channel msgs only) */
    uint8_t in_sysex, sx_n, sx_buf[3];        /* SysEx in progress: bytes waiting for a USB packet */
    uint32_t sx_len;                          /* SysEx bytes so far (monitor shows the total) */
    uint32_t packets, msgs, errors, sysex_done;
} ble_midi_t;
void ble_midi_reset(ble_midi_t *m);
void ble_midi_parse(ble_midi_t *m, const uint8_t *p, uint32_t n, ble_midi_out_fn out, void *ctx);
```

USB-MIDI packet format: byte 0 = `cable << 4 | CIN`, bytes 1..3 = MIDI bytes, as a little-endian `uint32_t`
(`cin | b1 << 8 | b2 << 16 | b3 << 24`), matching `usb.c`'s `midi_out_event`.

- [ ] **Step 1: Write the failing tests `tests/ble/ble_midi_test.c`**

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* ble_midi.c: BLE-MIDI packets (MMA / Apple) -> USB-MIDI event packets. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../firmware/src/ble/ble_midi.c"

static uint32_t got[64];
static int ngot;
static void out(void *ctx, uint32_t pkt) { (void)ctx; if (ngot < 64) got[ngot++] = pkt; }
static int fails;
#define CHECK(c, what) do { if (c) printf("ok    %s\n", what); else { printf("FAIL  %s\n", what); fails++; } } while (0)
#define PKT(cin, a, b, c) ((uint32_t)(cin) | (uint32_t)(a) << 8 | (uint32_t)(b) << 16 | (uint32_t)(c) << 24)

static void run(ble_midi_t *m, const uint8_t *p, uint32_t n) { ble_midi_parse(m, p, n, out, 0); }

int main(void)
{
    ble_midi_t m;
    {   /* one note on: header, timestamp, 90 3C 64 */
        static const uint8_t p[] = {0x80, 0x80, 0x90, 0x3C, 0x64};
        ble_midi_reset(&m); ngot = 0; run(&m, p, sizeof p);
        CHECK(ngot == 1 && got[0] == PKT(0x09, 0x90, 0x3C, 0x64), "single note on");
    }
    {   /* running status, the second message without status and without timestamp */
        static const uint8_t p[] = {0x80, 0x80, 0x90, 0x3C, 0x64, 0x3E, 0x64};
        ble_midi_reset(&m); ngot = 0; run(&m, p, sizeof p);
        CHECK(ngot == 2 && got[1] == PKT(0x09, 0x90, 0x3E, 0x64), "running status, no timestamp");
    }
    {   /* running status with a timestamp before the second message */
        static const uint8_t p[] = {0x80, 0x80, 0xB0, 0x07, 0x40, 0x81, 0x07, 0x41};
        ble_midi_reset(&m); ngot = 0; run(&m, p, sizeof p);
        CHECK(ngot == 2 && got[1] == PKT(0x0B, 0xB0, 0x07, 0x41), "running status with timestamp");
    }
    {   /* several full messages, program change (2 bytes), pitch bend */
        static const uint8_t p[] = {0x80, 0x80, 0xC1, 0x05, 0x80, 0xE1, 0x00, 0x48};
        ble_midi_reset(&m); ngot = 0; run(&m, p, sizeof p);
        CHECK(ngot == 2 && got[0] == PKT(0x0C, 0xC1, 0x05, 0) && got[1] == PKT(0x0E, 0xE1, 0x00, 0x48),
              "program change then pitch bend");
    }
    {   /* timestamp low wraps (0xFF then 0x80): still two messages */
        static const uint8_t p[] = {0x80, 0xFF, 0x90, 0x3C, 0x64, 0x80, 0x80, 0x3C, 0x00};
        ble_midi_reset(&m); ngot = 0; run(&m, p, sizeof p);
        CHECK(ngot == 2 && got[1] == PKT(0x08, 0x80, 0x3C, 0x00), "timestamp wrap");
    }
    {   /* real-time byte between messages */
        static const uint8_t p[] = {0x80, 0x80, 0xF8, 0x80, 0x90, 0x3C, 0x64};
        ble_midi_reset(&m); ngot = 0; run(&m, p, sizeof p);
        CHECK(ngot == 2 && got[0] == PKT(0x0F, 0xF8, 0, 0) && got[1] == PKT(0x09, 0x90, 0x3C, 0x64), "real-time");
    }
    {   /* SysEx in one packet: F0 7E 7F 06 01 F7 */
        static const uint8_t p[] = {0x80, 0x80, 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0x81, 0xF7};
        ble_midi_reset(&m); ngot = 0; run(&m, p, sizeof p);
        CHECK(ngot == 2 && got[0] == PKT(0x04, 0xF0, 0x7E, 0x7F) && got[1] == PKT(0x07, 0x06, 0x01, 0xF7)
              && m.sysex_done == 1 && m.sx_len == 6, "sysex in one packet");
    }
    {   /* SysEx across two packets; the second starts with plain data; a note follows */
        static const uint8_t p1[] = {0x80, 0x80, 0xF0, 0x41, 0x10};
        static const uint8_t p2[] = {0x80, 0x42, 0x12, 0x81, 0xF7, 0x82, 0x90, 0x40, 0x7F};
        ble_midi_reset(&m); ngot = 0; run(&m, p1, sizeof p1); run(&m, p2, sizeof p2);
        CHECK(m.sysex_done == 1 && m.sx_len == 6 && ngot == 3 && got[0] == PKT(0x04, 0xF0, 0x41, 0x10)
              && got[1] == PKT(0x07, 0x42, 0x12, 0xF7) && got[2] == PKT(0x09, 0x90, 0x40, 0x7F),
              "sysex across packets, then a note");
    }
    {   /* malformed: no header bit, data with no status, empty */
        static const uint8_t bad1[] = {0x00, 0x80, 0x90, 0x3C, 0x64};
        static const uint8_t bad2[] = {0x80, 0x80, 0x3C, 0x64};
        ble_midi_reset(&m); ngot = 0;
        run(&m, bad1, sizeof bad1); run(&m, bad2, sizeof bad2); run(&m, bad1, 0); run(&m, bad1, 1);
        CHECK(ngot == 0 && m.errors >= 3, "malformed packets counted, nothing emitted");
    }
    {   /* a truncated message at the packet end is dropped, the next packet parses */
        static const uint8_t p1[] = {0x80, 0x80, 0x90, 0x3C};
        static const uint8_t p2[] = {0x80, 0x80, 0x90, 0x3D, 0x10};
        ble_midi_reset(&m); ngot = 0; run(&m, p1, sizeof p1); run(&m, p2, sizeof p2);
        CHECK(ngot == 1 && got[0] == PKT(0x09, 0x90, 0x3D, 0x10) && m.errors == 1, "truncated message dropped");
    }
    printf(fails ? "ble_midi: %d FAILED\n" : "ble_midi: all checks passed\n", fails);
    return fails != 0;
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `mkdir -p build/tests && cc -Wall -O1 -o build/tests/ble_midi_test tests/ble/ble_midi_test.c`
Expected: a compile error, `ble_midi.c` not found.

- [ ] **Step 3: Write `firmware/src/ble/ble_midi.h` and `ble_midi.c`**

`ble_midi.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* BLE-MIDI (MMA "MIDI over Bluetooth Low Energy" 1.0) packets -> USB-MIDI event packets, cable 0. */
#ifndef BLE_MIDI_H
#define BLE_MIDI_H
#include <stdint.h>
typedef void (*ble_midi_out_fn)(void *ctx, uint32_t usb_pkt);
typedef struct {
    uint8_t status, need, got, d0, running;
    uint8_t in_sysex, sx_n, sx_buf[3];
    uint32_t sx_len;
    uint32_t packets, msgs, errors, sysex_done;
} ble_midi_t;
void ble_midi_reset(ble_midi_t *m);
void ble_midi_parse(ble_midi_t *m, const uint8_t *p, uint32_t n, ble_midi_out_fn out, void *ctx);
#endif
```

`ble_midi.c`:

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* BLE-MIDI parser. A packet: header (1 0 t12..t7), then per message a timestamp byte (1 t6..t0) and the MIDI
 * bytes. Running status may drop the status byte, and then the timestamp too. Inside SysEx only F7, real-time and
 * timestamp bytes have bit 7 set; a SysEx can run across packets (the next packet's bytes after its header are
 * data until a timestamp + F7). Malformed input is counted in errors and dropped. */
#include "ble_midi.h"

static uint32_t midi_len(uint8_t st)            /* total bytes of a message with this status */
{
    if (st < 0xC0u || (st >= 0xE0u && st < 0xF0u))
        return 3;
    if (st < 0xE0u)
        return 2;                                 /* Cx, Dx */
    switch (st) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    default: return 1;                            /* F6 and the undefined F4 / F5 */
    }
}

static uint32_t cin_of(uint8_t st, uint32_t len)
{
    if (st < 0xF0u)
        return st >> 4;
    return len == 3u ? 3u : len == 2u ? 2u : 5u;  /* system common, 1..3 bytes */
}

void ble_midi_reset(ble_midi_t *m)
{
    uint8_t *p = (uint8_t *)m;
    uint32_t i;
    for (i = 0; i < sizeof *m; i++)
        p[i] = 0;
}

static void emit(ble_midi_t *m, ble_midi_out_fn out, void *ctx, uint8_t st, uint8_t a, uint8_t b)
{
    uint32_t len = midi_len(st);
    out(ctx, cin_of(st, len) | (uint32_t)st << 8 | (uint32_t)(len > 1u ? a : 0u) << 16 |
             (uint32_t)(len > 2u ? b : 0u) << 24);
    m->msgs++;
}

static void sx_byte(ble_midi_t *m, ble_midi_out_fn out, void *ctx, uint8_t b)
{
    m->sx_buf[m->sx_n++] = b;
    m->sx_len++;
    if (b == 0xF7u) {                             /* end: CIN 5/6/7 with 1/2/3 bytes */
        uint32_t cin = 4u + m->sx_n;
        out(ctx, cin | (uint32_t)m->sx_buf[0] << 8 | (m->sx_n > 1u ? (uint32_t)m->sx_buf[1] << 16 : 0u) |
                 (m->sx_n > 2u ? (uint32_t)m->sx_buf[2] << 24 : 0u));
        m->sx_n = 0;
        m->in_sysex = 0;
        m->sysex_done++;
        m->msgs++;
    } else if (m->sx_n == 3u) {                   /* start / continue: CIN 4 */
        out(ctx, 4u | (uint32_t)m->sx_buf[0] << 8 | (uint32_t)m->sx_buf[1] << 16 | (uint32_t)m->sx_buf[2] << 24);
        m->sx_n = 0;
    }
}

void ble_midi_parse(ble_midi_t *m, const uint8_t *p, uint32_t n, ble_midi_out_fn out, void *ctx)
{
    uint32_t i, want_ts;
    if (n < 2u || (p[0] & 0xC0u) != 0x80u) {
        m->errors++;
        return;
    }
    m->packets++;
    want_ts = !m->in_sysex;                       /* a SysEx continuation starts with data */
    if (m->need) {                                /* a message cut off at the last packet's end */
        m->errors++;
        m->need = 0;
    }
    for (i = 1; i < n; i++) {
        uint8_t b = p[i];
        if (m->in_sysex) {
            if (!(b & 0x80u)) {
                sx_byte(m, out, ctx, b);
            } else if (b == 0xF7u) {
                sx_byte(m, out, ctx, b);
                want_ts = 1;
            } else if (b >= 0xF8u) {
                emit(m, out, ctx, b, 0, 0);       /* real-time inside SysEx */
            }                                      /* else: a timestamp inside SysEx */
            continue;
        }
        if (b & 0x80u) {
            if (want_ts) {                        /* timestamp byte */
                want_ts = 0;
                continue;
            }
            if (b >= 0xF8u) {                     /* real-time: one byte, keeps running status */
                emit(m, out, ctx, b, 0, 0);
                want_ts = 1;
                continue;
            }
            if (b == 0xF0u) {
                m->in_sysex = 1;
                m->sx_n = 0;
                m->sx_len = 0;
                sx_byte(m, out, ctx, b);
                continue;
            }
            m->status = b;
            m->running = b < 0xF0u ? b : 0u;
            m->need = (uint8_t)(midi_len(b) - 1u);
            m->got = 0;
            if (!m->need) {
                emit(m, out, ctx, b, 0, 0);
                want_ts = 1;
            }
            continue;
        }
        if (!m->need) {                           /* data with no message open: running status */
            if (!m->running) {
                m->errors++;
                continue;
            }
            m->status = m->running;
            m->need = (uint8_t)(midi_len(m->status) - 1u);
            m->got = 0;
            want_ts = 0;
        }
        if (m->got == 0u)
            m->d0 = b;
        m->got++;
        if (m->got == m->need) {
            emit(m, out, ctx, m->status, m->d0, b);
            m->need = 0;
            want_ts = 1;
        }
    }
}
```

- [ ] **Step 4: Run the tests**

Run: `cc -Wall -O1 -o build/tests/ble_midi_test tests/ble/ble_midi_test.c && build/tests/ble_midi_test`
Expected: every line `ok`, then `ble_midi: all checks passed`.

- [ ] **Step 5: Add to `tests/run_tests.sh` and commit**

```sh
run "BLE-MIDI parser" sh -c 'cc -Wall -O1 -o build/tests/ble_midi_test tests/ble/ble_midi_test.c && build/tests/ble_midi_test'
```

```bash
git add firmware/src/ble/ble_midi.h firmware/src/ble/ble_midi.c tests/ble/ble_midi_test.c tests/run_tests.sh
git commit -m "ble: BLE-MIDI packet parser to USB-MIDI events, host-tested"
```

---

### Task 4: Scan results table (host-tested)

**Files:**
- Create: `firmware/src/ble/ble_scan.h`, `firmware/src/ble/ble_scan.c`, `tests/ble/ble_scan_test.c`
- Modify: `tests/run_tests.sh`

**Interfaces:**
- Produces:

```c
#define BLE_SCAN_MAX 16
typedef struct {
    uint8_t addr[6], addr_type, midi, used;
    int8_t rssi;
    char name[24];                                /* 0-terminated; empty if none seen */
} ble_dev_t;
typedef struct { ble_dev_t dev[BLE_SCAN_MAX]; uint32_t reports; } ble_scan_t;
void ble_scan_clear(ble_scan_t *s);
void ble_scan_report(ble_scan_t *s, const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                     const uint8_t *ad, uint32_t ad_len);
uint32_t ble_scan_sorted(const ble_scan_t *s, uint8_t idx[BLE_SCAN_MAX]);   /* count; strongest first */
```

- [ ] **Step 1: Write the failing tests `tests/ble/ble_scan_test.c`**

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../firmware/src/ble/ble_scan.c"

static int fails;
#define CHECK(c, what) do { if (c) printf("ok    %s\n", what); else { printf("FAIL  %s\n", what); fails++; } } while (0)
static const uint8_t MIDI_UUID_LE[16] = {0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
                                         0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03};

int main(void)
{
    static ble_scan_t s;
    uint8_t a[6] = {1, 2, 3, 4, 5, 6}, idx[BLE_SCAN_MAX];
    uint8_t ad[40];
    uint32_t n = 0, k, cnt;
    /* flags + complete name "SMC-Mixer" + complete 128-bit list with the MIDI service */
    ad[n++] = 2; ad[n++] = 0x01; ad[n++] = 0x06;
    ad[n++] = 10; ad[n++] = 0x09; memcpy(ad + n, "SMC-Mixer", 9); n += 9;
    ad[n++] = 17; ad[n++] = 0x07; memcpy(ad + n, MIDI_UUID_LE, 16); n += 16;
    ble_scan_clear(&s);
    ble_scan_report(&s, a, 0, -60, ad, n);
    cnt = ble_scan_sorted(&s, idx);
    CHECK(cnt == 1 && !strcmp(s.dev[idx[0]].name, "SMC-Mixer") && s.dev[idx[0]].midi && s.dev[idx[0]].rssi == -60,
          "name, MIDI tag, rssi from one report");
    /* the same address again, name-less scan response: no duplicate, name and tag kept, rssi updated */
    ble_scan_report(&s, a, 0, -50, ad, 0);
    cnt = ble_scan_sorted(&s, idx);
    CHECK(cnt == 1 && !strcmp(s.dev[idx[0]].name, "SMC-Mixer") && s.dev[idx[0]].midi && s.dev[idx[0]].rssi == -50,
          "same address: merged, not duplicated");
    /* a scan response that only carries a shortened name merges into an entry first seen without a name */
    {
        uint8_t b[6] = {9, 9, 9, 9, 9, 9}, sr[8] = {5, 0x08, 'K', 'E', 'Y', 'S'};
        ble_scan_report(&s, b, 1, -70, ad, 3);    /* flags only */
        ble_scan_report(&s, b, 1, -70, sr, 6);
        cnt = ble_scan_sorted(&s, idx);
        CHECK(cnt == 2 && !strcmp(s.dev[idx[1]].name, "KEYS") && !s.dev[idx[1]].midi, "scan response name merged");
    }
    /* fill to 16 with weak devices, then a strong new one replaces the weakest */
    for (k = 0; k < 20; k++) {
        uint8_t c[6] = {0xAA, (uint8_t)k, 0, 0, 0, 0};
        ble_scan_report(&s, c, 0, (int8_t)(-90 - (int)(k % 5)), ad, 3);
    }
    cnt = ble_scan_sorted(&s, idx);
    CHECK(cnt == BLE_SCAN_MAX, "capped at 16");
    {
        uint8_t c[6] = {0xBB, 0, 0, 0, 0, 0};
        ble_scan_report(&s, c, 0, -40, ad, 3);
        cnt = ble_scan_sorted(&s, idx);
        CHECK(cnt == BLE_SCAN_MAX && s.dev[idx[0]].addr[0] == 0xBB, "a stronger device replaces the weakest");
        for (k = 1; k < cnt; k++)
            if (s.dev[idx[k - 1]].rssi < s.dev[idx[k]].rssi)
                break;
        CHECK(k == cnt, "sorted strongest first");
    }
    /* a malformed AD (length past the end) does not crash or read past */
    {
        uint8_t bad[4] = {30, 0x09, 'X', 'Y'}, d[6] = {7, 7, 7, 7, 7, 7};
        ble_scan_report(&s, d, 0, -30, bad, 4);
        cnt = ble_scan_sorted(&s, idx);
        CHECK(s.dev[idx[0]].addr[0] == 7 && s.dev[idx[0]].name[0] == 0, "malformed AD ignored safely");
    }
    CHECK(s.reports == 25, "reports counted");
    printf(fails ? "ble_scan: %d FAILED\n" : "ble_scan: all checks passed\n", fails);
    return fails != 0;
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cc -Wall -O1 -o build/tests/ble_scan_test tests/ble/ble_scan_test.c`
Expected: compile error (no `ble_scan.c`).

- [ ] **Step 3: Write `ble_scan.h` and `ble_scan.c`**

`ble_scan.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
#ifndef BLE_SCAN_H
#define BLE_SCAN_H
#include <stdint.h>
#define BLE_SCAN_MAX 16
typedef struct {
    uint8_t addr[6], addr_type, midi, used;
    int8_t rssi;
    char name[24];
} ble_dev_t;
typedef struct { ble_dev_t dev[BLE_SCAN_MAX]; uint32_t reports; } ble_scan_t;
void ble_scan_clear(ble_scan_t *s);
void ble_scan_report(ble_scan_t *s, const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                     const uint8_t *ad, uint32_t ad_len);
uint32_t ble_scan_sorted(const ble_scan_t *s, uint8_t idx[BLE_SCAN_MAX]);
#endif
```

`ble_scan.c`:

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* Scan results: one entry per address (advertising and scan response merge), up to BLE_SCAN_MAX; when full, a
 * stronger new device replaces the weakest. AD structures: [len][type][data len-1]; names 0x08 / 0x09; 128-bit
 * service lists 0x06 / 0x07 (little-endian UUIDs) for the BLE-MIDI tag. */
#include "ble_scan.h"

static const uint8_t MIDI_UUID_LE[16] = {0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
                                         0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03};

static int same6(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    for (i = 0; i < 6u; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

void ble_scan_clear(ble_scan_t *s)
{
    uint8_t *p = (uint8_t *)s;
    uint32_t i;
    for (i = 0; i < sizeof *s; i++)
        p[i] = 0;
}

static void parse_ad(ble_dev_t *d, const uint8_t *ad, uint32_t n)
{
    uint32_t i = 0;
    while (i + 1u < n) {
        uint32_t len = ad[i], type, k;
        if (!len || i + 1u + len > n)
            return;                               /* malformed: stop, keep what was parsed */
        type = ad[i + 1u];
        if ((type == 0x08u || type == 0x09u) && (type == 0x09u || !d->name[0])) {
            uint32_t m = len - 1u < sizeof d->name - 1u ? len - 1u : sizeof d->name - 1u;
            for (k = 0; k < m; k++) {
                uint8_t c = ad[i + 2u + k];
                d->name[k] = (char)(c >= 0x20u && c < 0x7Fu ? c : '?');
            }
            d->name[m] = 0;
        } else if (type == 0x06u || type == 0x07u) {
            for (k = 0; k + 16u <= len - 1u; k += 16u) {
                uint32_t j, eq = 1;
                for (j = 0; j < 16u; j++)
                    eq &= ad[i + 2u + k + j] == MIDI_UUID_LE[j];
                if (eq)
                    d->midi = 1;
            }
        }
        i += 1u + len;
    }
}

void ble_scan_report(ble_scan_t *s, const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                     const uint8_t *ad, uint32_t ad_len)
{
    ble_dev_t *d = 0, *weak = 0;
    uint32_t i;
    s->reports++;
    for (i = 0; i < BLE_SCAN_MAX; i++) {
        ble_dev_t *e = &s->dev[i];
        if (e->used && e->addr_type == addr_type && same6(e->addr, addr)) {
            d = e;
            break;
        }
    }
    if (!d) {
        for (i = 0; i < BLE_SCAN_MAX && !d; i++)
            if (!s->dev[i].used)
                d = &s->dev[i];
        if (!d) {
            for (i = 0; i < BLE_SCAN_MAX; i++)
                if (!weak || s->dev[i].rssi < weak->rssi)
                    weak = &s->dev[i];
            if (weak->rssi >= rssi)
                return;                           /* full and not stronger than the weakest */
            d = weak;
        }
        {
            uint8_t *p = (uint8_t *)d;
            for (i = 0; i < sizeof *d; i++)
                p[i] = 0;
        }
        for (i = 0; i < 6u; i++)
            d->addr[i] = addr[i];
        d->addr_type = addr_type;
        d->used = 1;
    }
    d->rssi = rssi;
    parse_ad(d, ad, ad_len);
}

uint32_t ble_scan_sorted(const ble_scan_t *s, uint8_t idx[BLE_SCAN_MAX])
{
    uint32_t n = 0, i, j;
    for (i = 0; i < BLE_SCAN_MAX; i++)
        if (s->dev[i].used)
            idx[n++] = (uint8_t)i;
    for (i = 1; i < n; i++)                       /* insertion sort, strongest first */
        for (j = i; j > 0u && s->dev[idx[j - 1u]].rssi < s->dev[idx[j]].rssi; j--) {
            uint8_t t = idx[j];
            idx[j] = idx[j - 1u];
            idx[j - 1u] = t;
        }
    return n;
}
```

- [ ] **Step 4: Run the tests**

Run: `cc -Wall -O1 -o build/tests/ble_scan_test tests/ble/ble_scan_test.c && build/tests/ble_scan_test`
Expected: all `ok`, then `ble_scan: all checks passed`.

- [ ] **Step 5: Add to `tests/run_tests.sh` and commit**

```sh
run "BLE scan table" sh -c 'cc -Wall -O1 -o build/tests/ble_scan_test tests/ble/ble_scan_test.c && build/tests/ble_scan_test'
```

```bash
git add firmware/src/ble/ble_scan.h firmware/src/ble/ble_scan.c tests/ble/ble_scan_test.c tests/run_tests.sh
git commit -m "ble: scan results table (dedup, merge, strongest 16, BLE-MIDI tag), host-tested"
```

---

### Task 5: SDK libraries, pins, and the link reconnaissance

**Files:**
- Create: `tools/ble_libs.py`, `firmware/src/ble/ble_sdk_abi.h` (generated, committed), `docs/ble/LINK_NOTES.md`

**Interfaces:**
- Consumes: the env var `AC79_BT_SDK` (a full SDK copy; default
  `~/Downloads/fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0`).
- Produces:
  - `tools/ble_libs.py libs` prints the pinned library paths, one per line. It fails if a hash differs.
  - `tools/ble_libs.py abi` writes `firmware/src/ble/ble_sdk_abi.h` with:

    ```c
    #define BLE_SDK_OS_SEM_SIZE     /* sizeof(OS_SEM) == sizeof(OS_MUTEX), bytes */
    #define BLE_SDK_OS_QUEUE_SIZE   /* sizeof(OS_QUEUE) */
    #define BLE_SDK_OS_TASKQ 13
    #define BLE_SDK_OS_TIMEOUT 11
    #define BLE_SDK_OS_Q_FULL 21
    #define BLE_SDK_OS_NO_ERR 0
    #define BLE_SDK_Q_MSG 0x100000
    #define BLE_SDK_Q_EVENT 0x200000
    #define BLE_SDK_Q_CALLBACK 0x300000
    #define BLE_SDK_Q_USER 0x400000
    #define BLE_SDK_IRQ_BLE_RX 29
    #define BLE_SDK_IRQ_BLE_EVENT 45
    ```

  - `docs/ble/LINK_NOTES.md` records:
    - the undefined symbols when linking the libraries against the core;
    - the archive members pulled in;
    - which IRQ handlers the controller passes to `request_irq` and whether they end in `rti`;
    - the semantics of `os_taskq_pend` / `os_taskq_post_type` / `Q_CALLBACK` read from `system.a`'s
      `os_api.c.o` IR.

- [ ] **Step 1: Write `tools/ble_libs.py`**

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI test: 2026 DEADACTIVE
"""The JieLi AC79 SDK libraries the BLE build links (Apache-2.0, JieLi; prebuilt LLVM bitcode), pinned by sha256.

  ble_libs.py libs     the library paths (fails on a hash mismatch)
  ble_libs.py pin      print the current hashes (to fill PINS the first time)
  ble_libs.py abi      write firmware/src/ble/ble_sdk_abi.h (sizes from the SDK headers, via the JieLi clang)
Env: AC79_BT_SDK = a full SDK copy (default ~/Downloads/fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0)."""
import hashlib
import os
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent
SDK = Path(os.environ.get("AC79_BT_SDK", "~/Downloads/fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0")).expanduser()
LIBS = ["cpu/wl82/liba/btctrler.a", "cpu/wl82/liba/btstack.a", "cpu/wl82/liba/wl_rf_common.a",
        "cpu/wl82/liba/crypto_toolbox_Osize.a"]
PINS = {}          # filled in step 2 from `ble_libs.py pin`, then never edited by hand


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def libs():
    out = []
    for rel in LIBS:
        p = SDK / rel
        if not p.exists():
            sys.exit(f"ble_libs: {p} missing (set AC79_BT_SDK to a full AC79 SDK copy)")
        if rel in PINS and sha(p) != PINS[rel]:
            sys.exit(f"ble_libs: {rel} sha256 {sha(p)} is not the pinned {PINS[rel]}")
        if rel not in PINS:
            sys.exit(f"ble_libs: {rel} not pinned (run `ble_libs.py pin` and fill PINS)")
        out.append(p)
    return out


def abi():
    probe = SRC / "build" / "ble_abi_probe.c"
    probe.parent.mkdir(exist_ok=True)
    probe.write_text('#define CONFIG_FREE_RTOS_ENABLE 1\n#include "generic/typedef.h"\n#include "os/os_api.h"\n'
                     "char sz_sem[sizeof(OS_SEM)]; char sz_mutex[sizeof(OS_MUTEX)]; char sz_queue[sizeof(OS_QUEUE)];\n"
                     "char v_taskq[OS_TASKQ]; char v_timeout[OS_TIMEOUT]; char v_qfull[OS_Q_FULL];\n")
    tc = Path(os.environ.get("JIELI_TOOLCHAIN", "~/.jieli/toolchain")).expanduser()
    inc = [f"-I/sdk/{d}" for d in ("include_lib", "include_lib/system", "include_lib/system/os",
                                    "include_lib/system/os/FreeRTOS", "include_lib/system/os/FreeRTOS/pi32v2",
                                    "include_lib/driver/cpu/wl82")]
    cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SRC}:/work", "-v", f"{SDK}:/sdk:ro",
           "-v", f"{tc}:/opt/jieli:ro", "-w", "/work", "debian:bookworm-slim", "/opt/jieli/pi32v2/bin/clang",
           "-target", "pi32v2", "-c", *inc, "build/ble_abi_probe.c", "-o", "build/ble_abi_probe.o"]
    subprocess.run(cmd, check=True)
    nm = subprocess.run(["xcrun", "llvm-nm", "-S", str(SRC / "build/ble_abi_probe.o")], capture_output=True,
                        text=True, check=True).stdout
    size = {ln.split()[3]: int(ln.split()[1], 16) for ln in nm.splitlines() if len(ln.split()) == 4}
    assert size["sz_sem"] == size["sz_mutex"], "OS_SEM and OS_MUTEX differ in size"
    assert (size["v_taskq"], size["v_timeout"], size["v_qfull"]) == (13, 11, 21), "OS return codes changed"
    (SRC / "firmware/src/ble/ble_sdk_abi.h").write_text(f"""/* Generated by tools/ble_libs.py abi from the AC79 SDK headers (do not edit) */
#define BLE_SDK_OS_SEM_SIZE {size['sz_sem']}u
#define BLE_SDK_OS_QUEUE_SIZE {size['sz_queue']}u
#define BLE_SDK_OS_NO_ERR 0
#define BLE_SDK_OS_TIMEOUT 11
#define BLE_SDK_OS_TASKQ 13
#define BLE_SDK_OS_Q_FULL 21
#define BLE_SDK_Q_MSG 0x100000
#define BLE_SDK_Q_EVENT 0x200000
#define BLE_SDK_Q_CALLBACK 0x300000
#define BLE_SDK_Q_USER 0x400000
#define BLE_SDK_IRQ_BLE_RX 29
#define BLE_SDK_IRQ_BLE_EVENT 45
""")
    print("wrote firmware/src/ble/ble_sdk_abi.h", size)


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "libs"
    if what == "pin":
        for rel in LIBS:
            print(f'    "{rel}": "{sha(SDK / rel)}",')
    elif what == "abi":
        abi()
    else:
        print("\n".join(str(p) for p in libs()))
```

- [ ] **Step 2: Pin the libraries**

Run: `python3 tools/ble_libs.py pin`. Paste its four lines into `PINS = { ... }` in `tools/ble_libs.py`.
Then `python3 tools/ble_libs.py libs` prints four paths and exits 0.

- [ ] **Step 3: Generate `ble_sdk_abi.h`**

Run: `python3 tools/ble_libs.py abi`
Expected: `wrote firmware/src/ble/ble_sdk_abi.h {'sz_sem': N, ...}`. `N` is the FreeRTOS `StaticSemaphore_t` size
(expected between 64 and 96).

If the include paths are wrong, the clang error names the missing header. Add its directory to `inc` and rerun.

- [ ] **Step 4: Link reconnaissance (no firmware is produced for the device)**

Read the IR of the OS calls and the IRQ registration, then record the results in `docs/ble/LINK_NOTES.md`:

```bash
mkdir -p build/sdkir && cd build/sdkir
xcrun llvm-ar x "$AC79_BT_SDK/cpu/wl82/liba/system.a" os_api.c.o task.c.o 2>/dev/null || \
  /opt/homebrew/opt/llvm@17/bin/llvm-ar x "$AC79_BT_SDK/cpu/wl82/liba/system.a" os_api.c.o task.c.o
/opt/homebrew/opt/llvm@17/bin/llvm-dis --disable-auto-upgrade-debug-info os_api.c.o -o os_api.ll
/opt/homebrew/opt/llvm@17/bin/llvm-dis --disable-auto-upgrade-debug-info task.c.o -o task.ll
grep -n "define .*@os_taskq_pend\|define .*@os_taskq_post_type\|define .*@__os_taskq_pend\|define .*@task_create" os_api.ll task.ll
cd ../..
```

(If `llvm@17` is missing: `brew install llvm@17`.)

From the IR, write down in `LINK_NOTES.md`:
- **`os_taskq_pend`:** what it writes into `argv` (expected: `argv[0]` = the type word, `Q_MSG | n` / `Q_EVENT` /
  `Q_CALLBACK`, then the arguments) and what it returns (`OS_TASKQ` = 13).
- **`Q_CALLBACK` messages:** does the pend run the callback itself, or does the caller do it (expected: the btstack
  and btctrler task loops call `argv[1]` with the following arguments)?
- **`task_create`:** how it uses `task_info_table` (name → prio, stack words, qsize words).

Then link the libraries against the core with an empty stub file. That lists what is missing:

```bash
FELUCCA_BLE=1 FELUCCA_BLE_STEP=0 PYTHON=$VENV/bin/python ./build.sh 2>&1 | grep -i "undefined" | sed 's/.*undefined symbol: //' | sort -u > docs/ble/undefined-step0.txt
wc -l docs/ble/undefined-step0.txt
```

This needs Task 6, step 1's `build.py` change. Do step 4 after that step, or add the `build.py` part of Task 6
first. Commit the list and the notes.

- [ ] **Step 5: Commit**

```bash
git add tools/ble_libs.py firmware/src/ble/ble_sdk_abi.h docs/ble/LINK_NOTES.md docs/ble/undefined-step0.txt
git commit -m "ble: pinned SDK BT libraries, SDK ABI sizes, link notes"
```

---

### Task 6: The OS layer (`ble_os`) and context switch

**Files:**
- Create: `firmware/hal/fm1_ctx.h`, `firmware/hal/fm1_ctx.S`, `firmware/src/ble/ble_os.h`,
  `firmware/src/ble/ble_os.c`, `tests/ble/ble_os_test.c`
- Modify: `tools/build.py` (assemble `fm1_ctx.S`; `FELUCCA_BLE` and `FELUCCA_BLE_STEP` flags; link the libraries
  when `FELUCCA_BLE=1`)
- Modify: `tests/run_tests.sh`

**Interfaces:**
- Consumes: `ble_sdk_abi.h` (Task 5). The `os_taskq_*` semantics are recorded in `docs/ble/LINK_NOTES.md`.
- Produces (C, used by `ble_port.c` / `ble_central.c` and called by the JieLi libraries):

```c
/* tasks */
int task_create(void (*task)(void *p), void *p, const char *name);       /* SDK signature */
const char *os_current_task(void);
const char *os_current_task_rom(void);
void os_time_dly(int ticks);                                             /* 1 tick = 10 ms (OS_TICKS_PER_SEC 100) */
/* task queues */
int os_taskq_post_type(const char *name, int type, int argc, int *argv); /* OS_NO_ERR or OS_Q_FULL */
int os_taskq_pend(const char *fmt, int *argv, int argc);                 /* blocks; returns OS_TASKQ */
/* semaphores / mutexes (OS_SEM, OS_MUTEX are opaque, BLE_SDK_OS_SEM_SIZE bytes) */
int os_sem_create(void *sem, int cnt);
int os_sem_pend(void *sem, int timeout_ticks);                           /* 0 = forever; OS_NO_ERR / OS_TIMEOUT */
int os_sem_post(void *sem);
int os_sem_set(void *sem, unsigned short cnt);
int os_mutex_create(void *m);
int os_mutex_pend(void *m, int timeout_ticks);
int os_mutex_post(void *m);
int os_mutex_del(void *m, int force);
/* timers (run in the task that drains them: here from ble_service()) */
unsigned short sys_timer_add(void *priv, void (*func)(void *priv), unsigned int msec);
unsigned short sys_timeout_add(void *priv, void (*func)(void *priv), unsigned int msec);
void sys_timer_del(unsigned short id);
void sys_timeout_del(unsigned short id);
int sys_timer_modify(unsigned short id, unsigned int msec);
/* heap */
void *malloc(unsigned int n);
void *zalloc(unsigned int n);
void free(void *p);
/* service */
void ble_os_init(uint8_t *heap, uint32_t heap_len);
uint32_t ble_os_service(uint32_t budget_us);   /* run ready tasks / due timers; returns tasks run */
uint32_t ble_os_now_ms(void);                  /* provided by the target (fm1_ms) or the host test */
```

- [ ] **Step 1: `build.py` flags**

In `build_app()`:
- Add `"FELUCCA_BLE"` to the flag loop.
- Accept `FELUCCA_BLE_STEP` (0..5):

```python
    v = os.environ.get("FELUCCA_BLE_STEP")
    if v in ("0", "1", "2", "3", "4", "5"):
        flags.append(f"-DFELUCCA_BLE_STEP={v}")
```

- Assemble `firmware/hal/fm1_ctx.S` like `fm1_isr.S` (same `tc("cc", ...)` call, output `OUT / "fm1_ctx.o"`).
- Add it to the link inputs.
- When `os.environ.get("FELUCCA_BLE") == "1"`, append the paths from `subprocess.run([sys.executable,
  "tools/ble_libs.py", "libs"], ...)` to the link inputs (each library mounted into the container: copy them into
  `build/blelibs/` first, so the Docker `-v SRC:/work` mount sees them).

Default builds (no `FELUCCA_BLE`) must stay byte-identical. Check: build without the flag and run
`python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref` → `unchanged`, and
`cmp build/felucca.bin <the Task 1 build>` (save it as `build/core_ref.bin` in Task 1, step 3).

- [ ] **Step 2: Context switch `firmware/hal/fm1_ctx.h` / `fm1_ctx.S`**

`fm1_ctx.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* Cooperative context switch (pi32v2). A context is a saved stack pointer; callee-saved r4-r15 and rets are on
 * its stack (the compiler's own prologue set: [--sp] = {rets, r15-r4}).
 *   fm1_ctx_switch(&from_sp, to_sp)       save here, resume `to`
 *   fm1_ctx_start(&from_sp, stack_top, entry, arg)   save here, run entry(arg) on a new stack (entry never returns) */
#pragma once
#include <stdint.h>
void fm1_ctx_switch(uint32_t *save_sp, uint32_t to_sp);
void fm1_ctx_start(uint32_t *save_sp, uint32_t stack_top, void (*entry)(void *), void *arg);
```

`fm1_ctx.S`:

```asm
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
	.section .text.fm1_ctx_switch,"ax",@progbits
	.globl fm1_ctx_switch
fm1_ctx_switch:                     /* r0 = &save_sp, r1 = to_sp */
	[--sp] = {rets, r15-r4}
	r2 = sp
	[r0 + 0] = r2
	sp = r1
	{pc, r15-r4} = [sp++]

	.section .text.fm1_ctx_start,"ax",@progbits
	.globl fm1_ctx_start
fm1_ctx_start:                      /* r0 = &save_sp, r1 = stack_top, r2 = entry, r3 = arg */
	[--sp] = {rets, r15-r4}
	r4 = sp
	[r0 + 0] = r4
	sp = r1
	r0 = r3
	call r2                         /* entry(arg); a task body never returns */
1:	goto 1b
```

Build check: `./build.sh` assembles it. If the assembler rejects a form, use the form `felucca.dis` shows for the
same operation (`grep "= sp\|sp = \|call r" build/felucca.dis | head`), keeping the semantics above.

- [ ] **Step 3: Write the failing host test `tests/ble/ble_os_test.c`**

The host back end uses `ucontext` (`-D BLE_OS_HOST`) instead of `fm1_ctx_*`. `ble_os_now_ms()` is a test clock.

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
#define BLE_OS_HOST 1
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint32_t test_ms;
uint32_t ble_os_now_ms(void) { return test_ms; }
#include "../../firmware/src/ble/ble_os.c"

static int fails;
#define CHECK(c, what) do { if (c) printf("ok    %s\n", what); else { printf("FAIL  %s\n", what); fails++; } } while (0)
static uint8_t heap[16384];
static int log_[32], nlog;
static uint8_t sem[BLE_SDK_OS_SEM_SIZE];

static void consumer(void *p)                   /* pends messages, logs their first argument */
{
    int argv[4];
    (void)p;
    for (;;) {
        int r = os_taskq_pend("taskq", argv, 4);
        if (r == BLE_SDK_OS_TASKQ && nlog < 32)
            log_[nlog++] = argv[1];
    }
}
static void waiter(void *p)                     /* waits on the semaphore with a 5-tick timeout, logs result */
{
    (void)p;
    for (;;) {
        int r = os_sem_pend(sem, 5);
        if (nlog < 32)
            log_[nlog++] = 1000 + r;
    }
}
static int fired;
static void tick(void *p) { (void)p; fired++; }
static void hog(void *p) { (void)p; for (;;) os_time_dly(0); }   /* yields without sleeping */

int main(void)
{
    int a[2];
    ble_os_init(heap, sizeof heap);
    task_create(consumer, 0, "btstack");
    ble_os_service(1000);
    a[0] = 7; os_taskq_post_type("btstack", BLE_SDK_Q_MSG, 1, a);
    a[0] = 8; os_taskq_post_type("btstack", BLE_SDK_Q_MSG, 1, a);
    ble_os_service(1000);
    CHECK(nlog == 2 && log_[0] == 7 && log_[1] == 8, "queue: posted messages arrive in order");

    nlog = 0;
    os_sem_create(sem, 0);
    task_create(waiter, 0, "btctrler");
    ble_os_service(1000);
    os_sem_post(sem);
    ble_os_service(1000);
    CHECK(nlog == 1 && log_[0] == 1000 + BLE_SDK_OS_NO_ERR, "semaphore post wakes the waiter");
    test_ms += 60;                              /* 5 ticks = 50 ms */
    ble_os_service(1000);
    CHECK(nlog == 2 && log_[1] == 1000 + BLE_SDK_OS_TIMEOUT, "semaphore pend times out");

    {
        unsigned short id = sys_timer_add(0, tick, 20);
        test_ms += 20; ble_os_service(1000);
        test_ms += 20; ble_os_service(1000);
        CHECK(fired == 2, "periodic timer fires per period");
        sys_timer_del(id);
        test_ms += 40; ble_os_service(1000);
        CHECK(fired == 2, "deleted timer stops");
        sys_timeout_add(0, tick, 10);
        test_ms += 10; ble_os_service(1000);
        test_ms += 10; ble_os_service(1000);
        CHECK(fired == 3, "timeout fires once");
    }
    {
        void *p1 = malloc(3072), *p2 = malloc(512), *p3 = malloc(20000);
        CHECK(p1 && p2 && !p3, "heap: fits, refuses beyond the pool");
        free(p1);
        p3 = malloc(3000);
        CHECK(p3 == p1, "heap: freed block reused");
    }
    {
        uint32_t t0;
        task_create(hog, 0, "hog");
        t0 = test_ms;
        ble_os_service(50000);                  /* budget 50 ms: must return though hog never sleeps */
        CHECK(1, "service returns within budget with a task that never sleeps");
        (void)t0;
    }
    printf(fails ? "ble_os: %d FAILED\n" : "ble_os: all checks passed\n", fails);
    return fails != 0;
}
```

On the host `ble_os_service(budget_us)` bounds its work by a count of switches (`BLE_OS_HOST_SWITCHES = 64`),
because the test clock does not advance by itself. On the target it uses `fm1_ticks()`.

- [ ] **Step 4: Run it to see it fail**

Run: `cc -Wall -O1 -Ifirmware/src/ble -o build/tests/ble_os_test tests/ble/ble_os_test.c`
Expected: a compile error (no `ble_os.c`).

- [ ] **Step 5: Write `ble_os.h` / `ble_os.c`**

`ble_os.h`: the declarations from **Interfaces**, plus `#include "ble_sdk_abi.h"`.

`ble_os.c` (the full file):

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* A cooperative OS for the JieLi BT libraries. Tasks switch only inside the blocking calls below; the core's main
 * loop calls ble_os_service() to run them, so nothing ever preempts the core. Interrupt handlers may post to
 * queues and semaphores (they only mark the waiter ready). */
#include "ble_os.h"
#ifdef BLE_OS_HOST
#include <ucontext.h>
#include <stdlib.h>
#else
#include "fm1_ctx.h"
#endif

#define NTASK 4
#define QWORDS 96                                  /* message words per task queue */
#define NTIMER 16
#define STACK_WORDS 1024                           /* 4 KiB per task */

typedef struct {
    const char *name;
    void (*fn)(void *);
    void *arg;
    uint32_t q[QWORDS], qh, qt;                    /* ring of message words: [len, type, args...] */
    void *wait_obj;                                /* semaphore it waits on, or 0 */
    uint32_t wake_ms, waiting_q, sleeping, ready, started;
#ifdef BLE_OS_HOST
    ucontext_t ctx;
#else
    uint32_t sp;
#endif
    uint32_t stack[STACK_WORDS];
} task_t;

typedef struct { uint32_t used, periodic, period, due; void (*fn)(void *); void *priv; } tmr_t;
typedef struct { int16_t count; uint8_t magic, owner; } sem_t;   /* lives inside the SDK's OS_SEM buffer */
#define SEM_MAGIC 0x5Eu

static task_t tasks[NTASK] __attribute__((section(".pool")));
static tmr_t timers[NTIMER];
static int cur = -1;                               /* running task, -1 = the core */
#ifdef BLE_OS_HOST
static ucontext_t core_ctx;
#else
static uint32_t core_sp;
#endif
_Static_assert(sizeof(sem_t) <= BLE_SDK_OS_SEM_SIZE, "sem_t fits in OS_SEM");

/* ---- heap: first fit, 8-byte aligned, headers with a guard word ---- */
typedef struct blk { uint32_t size, used, guard; struct blk *next; } blk_t;
static blk_t *heap0;
#define GUARD 0xB1E0B1E0u

void ble_os_init(uint8_t *heap, uint32_t heap_len)
{
    uint32_t i;
    heap0 = (blk_t *)(void *)(((uintptr_t)heap + 7u) & ~(uintptr_t)7u);
    heap0->size = (uint32_t)(heap + heap_len - (uint8_t *)heap0) - sizeof(blk_t);
    heap0->used = 0;
    heap0->guard = GUARD;
    heap0->next = 0;
    for (i = 0; i < NTASK; i++)
        tasks[i].name = 0;
    for (i = 0; i < NTIMER; i++)
        timers[i].used = 0;
    cur = -1;
}

void *malloc(unsigned int n)
{
    blk_t *b;
    n = (n + 7u) & ~7u;
    for (b = heap0; b; b = b->next) {
        if (b->guard != GUARD)
            return 0;                              /* heap overrun detected: refuse, never crash */
        if (!b->used && b->size >= n) {
            if (b->size >= n + sizeof(blk_t) + 16u) {
                blk_t *r = (blk_t *)(void *)((uint8_t *)(b + 1) + n);
                r->size = b->size - n - sizeof(blk_t);
                r->used = 0;
                r->guard = GUARD;
                r->next = b->next;
                b->next = r;
                b->size = n;
            }
            b->used = 1;
            return b + 1;
        }
    }
    return 0;
}

void *zalloc(unsigned int n)
{
    uint8_t *p = malloc(n);
    uint32_t i;
    if (p)
        for (i = 0; i < n; i++)
            p[i] = 0;
    return p;
}

void free(void *p)
{
    blk_t *b, *f;
    if (!p)
        return;
    f = (blk_t *)p - 1;
    if (f->guard != GUARD)
        return;
    f->used = 0;
    for (b = heap0; b; b = b->next)               /* merge free neighbours */
        while (!b->used && b->next && !b->next->used) {
            b->size += sizeof(blk_t) + b->next->size;
            b->next = b->next->next;
        }
}

/* ---- tasks ---- */
static int find(const char *name)
{
    int i;
    for (i = 0; i < NTASK; i++) {
        const char *a = tasks[i].name, *b = name;
        if (!a)
            continue;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == *b)
            return i;
    }
    return -1;
}

static void trampoline(void *arg)
{
    task_t *t = arg;
    t->fn(t->arg);
    for (;;)
        os_time_dly(1000);                         /* a task body that returns parks here */
}

#ifdef BLE_OS_HOST
static void host_entry(void) { trampoline(&tasks[cur]); }
#endif

int task_create(void (*task)(void *p), void *p, const char *name)
{
    int i;
    for (i = 0; i < NTASK; i++)
        if (!tasks[i].name) {
            task_t *t = &tasks[i];
            t->name = name;
            t->fn = task;
            t->arg = p;
            t->qh = t->qt = 0;
            t->wait_obj = 0;
            t->waiting_q = t->sleeping = t->started = 0;
            t->ready = 1;
            return 0;
        }
    return -1;
}

const char *os_current_task(void) { return cur >= 0 ? tasks[cur].name : "app_core"; }
const char *os_current_task_rom(void) { return os_current_task(); }

static void yield(void)                            /* from a task back to the scheduler (the core) */
{
    int me = cur;
#ifdef BLE_OS_HOST
    swapcontext(&tasks[me].ctx, &core_ctx);
#else
    fm1_ctx_switch(&tasks[me].sp, core_sp);
#endif
}

static void run(int i)                             /* from the scheduler into task i */
{
    task_t *t = &tasks[i];
    cur = i;
    t->ready = 0;
#ifdef BLE_OS_HOST
    if (!t->started) {
        t->started = 1;
        getcontext(&t->ctx);
        t->ctx.uc_stack.ss_sp = t->stack;
        t->ctx.uc_stack.ss_size = sizeof t->stack;
        t->ctx.uc_link = &core_ctx;
        makecontext(&t->ctx, host_entry, 0);
    }
    swapcontext(&core_ctx, &t->ctx);
#else
    if (!t->started) {
        t->started = 1;
        fm1_ctx_start(&core_sp, (uint32_t)(uintptr_t)&t->stack[STACK_WORDS], trampoline, t);
    } else {
        fm1_ctx_switch(&core_sp, t->sp);
    }
#endif
    cur = -1;
}

void os_time_dly(int ticks)
{
    if (cur < 0)
        return;                                    /* never sleep the core */
    tasks[cur].sleeping = 1;
    tasks[cur].wake_ms = ble_os_now_ms() + (uint32_t)ticks * 10u;
    yield();
}

/* ---- queues: words [len, type, args...] ---- */
static uint32_t q_used(const task_t *t) { return t->qt - t->qh; }

int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    int i = find(name), k;
    task_t *t;
    if (i < 0)
        return BLE_SDK_OS_Q_FULL;
    t = &tasks[i];
    if (q_used(t) + 2u + (uint32_t)argc > QWORDS)
        return BLE_SDK_OS_Q_FULL;
    t->q[t->qt++ % QWORDS] = (uint32_t)argc;
    t->q[t->qt++ % QWORDS] = (uint32_t)type;
    for (k = 0; k < argc; k++)
        t->q[t->qt++ % QWORDS] = (uint32_t)argv[k];
    if (t->waiting_q)
        t->ready = 1;
    return BLE_SDK_OS_NO_ERR;
}

int os_taskq_pend(const char *fmt, int *argv, int argc)
{
    task_t *t;
    uint32_t n, k;
    (void)fmt;
    if (cur < 0)
        return BLE_SDK_OS_TIMEOUT;
    t = &tasks[cur];
    while (!q_used(t)) {
        t->waiting_q = 1;
        yield();
    }
    t->waiting_q = 0;
    n = t->q[t->qh++ % QWORDS];
    argv[0] = (int)t->q[t->qh++ % QWORDS];         /* type: Q_MSG / Q_EVENT / Q_CALLBACK / Q_USER */
    for (k = 0; k < n; k++) {
        uint32_t w = t->q[t->qh++ % QWORDS];
        if (k + 1u < (uint32_t)argc)
            argv[k + 1u] = (int)w;
    }
    return BLE_SDK_OS_TASKQ;
}

/* ---- semaphores / mutexes ---- */
int os_sem_create(void *s, int cnt)
{
    sem_t *x = s;
    x->count = (int16_t)cnt;
    x->magic = SEM_MAGIC;
    x->owner = 0;
    return BLE_SDK_OS_NO_ERR;
}

int os_sem_post(void *s)
{
    sem_t *x = s;
    int i;
    x->count++;
    for (i = 0; i < NTASK; i++)
        if (tasks[i].name && tasks[i].wait_obj == s)
            tasks[i].ready = 1;
    return BLE_SDK_OS_NO_ERR;
}

int os_sem_set(void *s, unsigned short cnt) { ((sem_t *)s)->count = (int16_t)cnt; return BLE_SDK_OS_NO_ERR; }

int os_sem_pend(void *s, int timeout)
{
    sem_t *x = s;
    uint32_t until = ble_os_now_ms() + (uint32_t)timeout * 10u;
    while (x->count <= 0) {
        if (cur < 0)
            return BLE_SDK_OS_TIMEOUT;             /* the core never blocks */
        if (timeout && (int32_t)(ble_os_now_ms() - until) >= 0) {
            tasks[cur].wait_obj = 0;
            return BLE_SDK_OS_TIMEOUT;
        }
        tasks[cur].wait_obj = s;
        tasks[cur].sleeping = timeout != 0;
        tasks[cur].wake_ms = until;
        yield();
    }
    tasks[cur >= 0 ? cur : 0].wait_obj = 0;
    x->count--;
    return BLE_SDK_OS_NO_ERR;
}

int os_mutex_create(void *m) { return os_sem_create(m, 1); }
int os_mutex_pend(void *m, int timeout) { return os_sem_pend(m, timeout); }
int os_mutex_post(void *m) { return os_sem_post(m); }
int os_mutex_del(void *m, int force) { (void)m; (void)force; return BLE_SDK_OS_NO_ERR; }

/* ---- timers ---- */
static unsigned short tmr_add(void *priv, void (*fn)(void *), unsigned int ms, uint32_t periodic)
{
    unsigned short i;
    for (i = 0; i < NTIMER; i++)
        if (!timers[i].used) {
            timers[i].used = 1;
            timers[i].periodic = periodic;
            timers[i].period = ms;
            timers[i].due = ble_os_now_ms() + ms;
            timers[i].fn = fn;
            timers[i].priv = priv;
            return (unsigned short)(i + 1u);
        }
    return 0;
}
unsigned short sys_timer_add(void *priv, void (*func)(void *priv), unsigned int msec) { return tmr_add(priv, func, msec, 1); }
unsigned short sys_timeout_add(void *priv, void (*func)(void *priv), unsigned int msec) { return tmr_add(priv, func, msec, 0); }
void sys_timer_del(unsigned short id) { if (id && id <= NTIMER) timers[id - 1u].used = 0; }
void sys_timeout_del(unsigned short id) { sys_timer_del(id); }
int sys_timer_modify(unsigned short id, unsigned int msec)
{
    if (!id || id > NTIMER || !timers[id - 1u].used)
        return -1;
    timers[id - 1u].period = msec;
    timers[id - 1u].due = ble_os_now_ms() + msec;
    return 0;
}

static uint32_t run_timers(void)
{
    uint32_t i, n = 0, now = ble_os_now_ms();
    for (i = 0; i < NTIMER; i++) {
        tmr_t *t = &timers[i];
        if (t->used && (int32_t)(now - t->due) >= 0) {
            void (*fn)(void *) = t->fn;
            void *priv = t->priv;
            if (t->periodic)
                t->due += t->period ? t->period : 1u;
            else
                t->used = 0;
            fn(priv);
            n++;
        }
    }
    return n;
}

/* ---- the scheduler, called from the core's main loop ---- */
#ifdef BLE_OS_HOST
#define BLE_OS_HOST_SWITCHES 64
#endif
uint32_t ble_os_service(uint32_t budget_us)
{
    uint32_t ran = 0, progress = 1;
#ifdef BLE_OS_HOST
    uint32_t switches = 0;
    (void)budget_us;
#else
    uint32_t t0 = fm1_ticks();
#endif
    run_timers();
    while (progress) {
        int i;
        progress = 0;
        for (i = 0; i < NTASK; i++) {
            task_t *t = &tasks[i];
            uint32_t now = ble_os_now_ms();
            if (!t->name)
                continue;
            if (t->sleeping && (int32_t)(now - t->wake_ms) >= 0) {
                t->sleeping = 0;
                t->ready = 1;
            }
            if (!t->ready && !(t->waiting_q && q_used(t)))
                continue;
            run(i);
            ran++;
            progress = 1;
#ifdef BLE_OS_HOST
            if (++switches >= BLE_OS_HOST_SWITCHES)
                return ran;
#else
            if (fm1_ticks() - t0 >= budget_us * FM1_TICKS_PER_US)
                return ran;
#endif
        }
    }
    return ran;
}
```

- [ ] **Step 6: Run the host test**

Run: `cc -Wall -O1 -Wno-deprecated-declarations -Ifirmware/src/ble -o build/tests/ble_os_test tests/ble/ble_os_test.c && build/tests/ble_os_test`
Expected: all `ok`, then `ble_os: all checks passed`.

macOS marks `ucontext` deprecated but it works. If it does not on this macOS, build the test in Docker
(`debian:bookworm-slim`, `apt-get install -y gcc`).

- [ ] **Step 7: Add to `tests/run_tests.sh` and commit**

```sh
run "BLE OS layer (host back end)" sh -c 'cc -Wall -O1 -Wno-deprecated-declarations -Ifirmware/src/ble -o build/tests/ble_os_test tests/ble/ble_os_test.c && build/tests/ble_os_test'
```

```bash
git add firmware/hal/fm1_ctx.h firmware/hal/fm1_ctx.S firmware/src/ble/ble_os.h firmware/src/ble/ble_os.c tests/ble/ble_os_test.c tools/build.py tests/run_tests.sh
git commit -m "ble: cooperative OS layer for the JieLi BT libraries (tasks, queues, semaphores, timers, heap); pi32v2 context switch"
```

---

### Task 7: The port layer and a clean link (`FELUCCA_BLE=1`, step 1 = linked, never started)

**Files:**
- Create: `firmware/src/ble/ble_port.c`, `firmware/src/ble/ble.c`
- Modify: `firmware/src/felucca.c` (one include), `firmware/src/core_main.c` (the `ble_service()` call)
- Create: `tools/check_ble_boot.py`
- Modify: `tests/run_tests.sh`

**Interfaces:**
- Consumes: `docs/ble/undefined-step0.txt` (Task 5); `ble_os.*` (Task 6).
- Produces:
  - `void ble_service(void);` (runs `ble_os_service(50000)` when started; returns at once otherwise);
  - `int ble_started(void);`;
  - a link with no undefined symbols when `FELUCCA_BLE=1`.

- [ ] **Step 1: `ble.c`, the module's unity file**

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* The BLE module, included by felucca.c when FELUCCA_BLE (after core_audio.c and console.c, before core_main.c).
 * FELUCCA_BLE_STEP: 1 linked only, 2 + start and scan, 3 + list, 4 + connect, 5 + monitor and USB forwarding. */
#ifndef FELUCCA_BLE_STEP
#define FELUCCA_BLE_STEP 1
#endif
uint32_t ble_os_now_ms(void) { return fm1_ms; }
#include "ble/ble_sdk_abi.h"
#include "ble/ble_os.c"
#include "ble/ble_midi.c"
#include "ble/ble_scan.c"
#include "ble/ble_port.c"
static uint8_t ble_heap[48 * 1024] __attribute__((section(".pool")));
static uint8_t ble_on;
int ble_started(void) { return ble_on; }
void ble_service(void)
{
    if (ble_on)
        ble_os_service(50000);                     /* at most ~50 ms per main-loop pass */
}
```

In `felucca.c`, after `#include "console.c"` / `#endif`, add:

```c
#if FELUCCA_BLE
#include "ble/ble.c"
#endif
```

In `core_main.c`'s loop, after `cdc_task();` / `#endif`, add:

```c
#if FELUCCA_BLE
        ble_service();
#endif
```

- [ ] **Step 2: `ble_port.c`, one definition per undefined symbol**

For each line of `docs/ble/undefined-step0.txt`, add a definition in one of these groups (comment each group with
why the stub is safe):
1. **The OS:** already in `ble_os.c`.
2. **`config_*` constants:** values from `apps/common/config/log_config/lib_btctrler_config.c` and
   `lib_btstack_config.c` of the SDK with `CONFIG_BT_ENABLE`, `TCFG_USER_BLE_ENABLE 1`, `BT_NET_CENTRAL_EN 1`,
   `TRANS_DATA_EN 0` (as the agent's demo build). For example:

   ```c
   const int config_btctler_modules = BT_MODULE_LE;                 /* (from btcontroller_modules.h) */
   const int config_btctler_le_roles = (LE_SCAN | LE_INIT | LE_MASTER);
   const int config_btctler_le_hw_nums = 1;
   const int config_le_gatt_client_num = 1;
   ```

   Copy the constant names, types and the macros' values from the SDK files (the macro headers are under
   `include_lib/btctrler/`). Record the source line for each in a comment.
3. **`log_tag_const_*`:** `const char log_tag_const_X = 0;` for each (logging off).
4. **Platform:**

   ```c
   void local_irq_disable(void) { fm1_ble_irq_off(); }
   void local_irq_enable(void) { fm1_ble_irq_on(); }
   void request_irq(unsigned char index, unsigned char priority, void (*handler)(void), unsigned char cpu_id)
   {
       (void)cpu_id;
       fm1_irq_attach(index, handler, priority);    /* see LINK_NOTES: handlers are interrupt functions */
   }
   ```

   `fm1_ble_irq_off` / `_on` go in `hal/fm1_irq.h` as the existing `cli` / `csync; sti` pair (no asm in `src/`).

   If `LINK_NOTES.md` says the controller's handlers do **not** end in `rti`, wrap them instead: add
   `FM1_ISR isr_ble_rx, ble_rx_body` / `isr_ble_ev, ble_ev_body` to `hal/fm1_isr.S`. The bodies call the saved
   handler pointers.
5. **Config reads, read-only:** `int syscfg_read(u16 id, void *buf, u16 len)`:
   - for ID 102 / `CFG_BT_MAC_ADDR`: a MAC derived from the flash chip's unique ID, with the top bits set to
     random-static (`mac[5] |= 0xC0`). The ID comes from `fm1_flash_uid()`, read-only command 0x4B in RAM code like
     `fl_jedec_ram`. It goes in a **new app-only header `hal/fm1_flash_uid.h`**, never in `hal/fm1_flash.h`: the
     loader includes `fm1_flash.h`, and a new `RAMFN` (it is `used`) would change `ota.bin` and fail gate 1.
   - every other ID: return 0 (not found), so the library uses its defaults;
   - `syscfg_write` returns -1 and writes nothing.
6. **Unused features** (classic BT, TWS, LE audio, LwIP, A2DP, HID): empty functions or zero constants. Each
   comment names the feature that is compiled out. If a symbol's role is unclear, look it up with
   `grep -rn NAME $AC79_BT_SDK/include_lib $AC79_BT_SDK/apps/common` before stubbing it.

- [ ] **Step 3: Build step 1 until it links**

Run: `FELUCCA_BLE=1 FELUCCA_BLE_STEP=1 PYTHON=$VENV/bin/python ./build.sh 2>&1 | tail -8`

Repeat step 2 until `ok    image ... B` appears. Expected: about 170–250 KB, RAM within limits, pool at least
8 KiB free.

- [ ] **Step 4: Gate 4, `tools/check_ble_boot.py`**

```python
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
```

Run: `python3 tools/check_ble_boot.py build/felucca.dis`
Expected: `no BT start-up reachable at boot ...` (in step 1 nothing calls the start functions at all).

- [ ] **Step 5: Gates 1–3 on the step-1 build**

```bash
shasum -a 256 build/loader/ota.bin    # e671adf0ce6e4f69ac8514087c8833b1969fa4c470645786f19968e90b9a630f
python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref   # update path: unchanged
tests/run_tests.sh 2>&1 | tail -3      # ALL HOST TESTS PASSED
```

If gate 2 reports a changed function, fix it before going on. Typical cause: a new call into a core static
function changed the compiler's inlining. Route the BLE code through a `noinline` wrapper instead of calling the
static function directly.

- [ ] **Step 6: Add gate 4 to `tests/run_tests.sh`, commit**

```sh
run "no BT start-up at boot (gate 4)" python3 tools/check_ble_boot.py build/felucca.dis
```

```bash
git add firmware/src/ble/ble.c firmware/src/ble/ble_port.c firmware/src/felucca.c firmware/src/core_main.c firmware/hal tools/check_ble_boot.py tests/run_tests.sh
git commit -m "ble: port layer; FELUCCA_BLE=1 links the JieLi BT libraries (step 1: linked, never started); gate 4"
```

---

### Task 8: Central app, screens and keys (steps 2–5 in code)

**Files:**
- Create: `firmware/src/ble/ble_central.c`, `firmware/src/ble/ble_ui.c`, `tests/ble/ble_central_test.c`
- Modify: `firmware/src/ble/ble.c` (include them; `ble_start`), `firmware/src/core_main.c` (key handler call),
  `firmware/src/console.c` (`ble` command)

**Interfaces:**
- Consumes: `ble_scan_*` (Task 4), `ble_midi_*` (Task 3), `ble_os_*` (Task 6). From the SDK (headers under
  `include_lib/btstack/le/`, linked in Task 7):
  - `btstack_init()`;
  - the `BT_STATUS_INIT_OK` event;
  - `le_device_db_init`, `ble_stack_gatt_role(1)`, `sm_init`, `gatt_client_init`,
    `gatt_client_register_packet_handler`, `hci_event_callback_set`, `le_l2cap_register_packet_handler`;
  - `ble_op_set_scan_param` / `ble_op_scan_enable2`, `GAP_EVENT_ADVERTISING_REPORT` (`adv_report_t`);
  - `ble_op_create_connection(struct create_conn_param_t *)`, `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`;
  - `ble_op_att_send_init`, `user_client_init` + `ble_op_search_profile_uuid128`;
  - `user_client_report_search_result(search_result_t *)`;
  - `ble_op_att_send_data(handle, data, len, ATT_OP_WRITE)` for the CCCD;
  - `user_client_report_data_callback(att_data_report_t *)` with `GATT_EVENT_NOTIFICATION`;
  - `HCI_EVENT_DISCONNECTION_COMPLETE`.

  The reference implementation of every call is `apps/common/ble/le_net_central.c` of the SDK: lines 712–727
  (connect), 842–950 (event handler), 386–415 (search result), 453–480 (data), 481–490 (search start), 1286–1330
  (profile init).
- Produces:
  - `void ble_start(void); void ble_scan(void); void ble_connect(uint32_t sorted_index); void ble_disconnect(void);`
  - `void ble_key(uint32_t pressed_buttons, int32_t select_steps);` (PLAY / REC / SELECT; the only entry from the
    core);
  - `void ble_draw(void);`
  - `void ble_status(void);` (console);
  - the state enum `BLE_OFF, BLE_STARTING, BLE_IDLE, BLE_SCANNING, BLE_CONNECTING, BLE_DISCOVERING, BLE_SUBSCRIBED,
    BLE_RECEIVING, BLE_FAILED`, with `ble_state()`;
  - `ble_err` (last error text and code).

The state machine (pure, host-tested in `ble_central_test.c`):

```c
typedef enum { EV_INIT_OK, EV_INIT_FAIL, EV_SCAN_DONE, EV_CONNECTED, EV_CONN_FAIL, EV_FOUND_MIDI, EV_NO_MIDI,
               EV_SUBSCRIBED, EV_NOTIFY, EV_DISCONNECTED, EV_TIMEOUT } ble_ev_t;
uint32_t ble_next(uint32_t state, ble_ev_t ev);   /* returns the new state; unknown pairs keep the state */
```

Transitions:

| From | Event | To |
| --- | --- | --- |
| STARTING | INIT_OK | IDLE |
| STARTING | INIT_FAIL / TIMEOUT | FAILED |
| SCANNING | SCAN_DONE | IDLE |
| CONNECTING | CONNECTED | DISCOVERING |
| CONNECTING | CONN_FAIL / TIMEOUT | IDLE |
| DISCOVERING | FOUND_MIDI | SUBSCRIBED (the CCCD write is sent on the transition) |
| DISCOVERING | NO_MIDI / TIMEOUT | IDLE (and a disconnect is issued) |
| SUBSCRIBED | NOTIFY | RECEIVING |
| RECEIVING | NOTIFY | RECEIVING |
| CONNECTING / DISCOVERING / SUBSCRIBED / RECEIVING | DISCONNECTED | IDLE |

- [ ] **Step 1: Write the failing state-machine test `tests/ble/ble_central_test.c`**

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
#include <stdio.h>
#include <stdint.h>
#define BLE_CENTRAL_STATE_ONLY 1
#include "../../firmware/src/ble/ble_central.c"
static int fails;
#define CHECK(c, what) do { if (c) printf("ok    %s\n", what); else { printf("FAIL  %s\n", what); fails++; } } while (0)
int main(void)
{
    static const uint32_t live[] = {BLE_CONNECTING, BLE_DISCOVERING, BLE_SUBSCRIBED, BLE_RECEIVING};
    uint32_t i;
    CHECK(ble_next(BLE_STARTING, EV_INIT_OK) == BLE_IDLE, "start ok -> idle");
    CHECK(ble_next(BLE_STARTING, EV_TIMEOUT) == BLE_FAILED, "start timeout -> failed");
    CHECK(ble_next(BLE_SCANNING, EV_SCAN_DONE) == BLE_IDLE, "scan done -> idle");
    CHECK(ble_next(BLE_CONNECTING, EV_CONNECTED) == BLE_DISCOVERING, "connected -> discovering");
    CHECK(ble_next(BLE_CONNECTING, EV_TIMEOUT) == BLE_IDLE, "connect timeout -> idle");
    CHECK(ble_next(BLE_DISCOVERING, EV_FOUND_MIDI) == BLE_SUBSCRIBED, "MIDI found -> subscribed");
    CHECK(ble_next(BLE_DISCOVERING, EV_NO_MIDI) == BLE_IDLE, "no MIDI service -> idle");
    CHECK(ble_next(BLE_SUBSCRIBED, EV_NOTIFY) == BLE_RECEIVING, "first notification -> receiving");
    for (i = 0; i < 4; i++)
        CHECK(ble_next(live[i], EV_DISCONNECTED) == BLE_IDLE, "disconnect in a live state -> idle");
    CHECK(ble_next(BLE_IDLE, EV_NOTIFY) == BLE_IDLE, "stray event keeps the state");
    printf(fails ? "ble_central: %d FAILED\n" : "ble_central: all checks passed\n", fails);
    return fails != 0;
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cc -Wall -O1 -o build/tests/ble_central_test tests/ble/ble_central_test.c`
Expected: compile error.

- [ ] **Step 3: Write `ble_central.c`**

At the top, the state machine (always compiled); the SDK glue below it under `#ifndef BLE_CENTRAL_STATE_ONLY`:

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* BLE-MIDI central on the JieLi SDK's ble_op_* API (reference: SDK apps/common/ble/le_net_central.c). */
enum { BLE_OFF, BLE_STARTING, BLE_IDLE, BLE_SCANNING, BLE_CONNECTING, BLE_DISCOVERING, BLE_SUBSCRIBED,
       BLE_RECEIVING, BLE_FAILED };
typedef enum { EV_INIT_OK, EV_INIT_FAIL, EV_SCAN_DONE, EV_CONNECTED, EV_CONN_FAIL, EV_FOUND_MIDI, EV_NO_MIDI,
               EV_SUBSCRIBED, EV_NOTIFY, EV_DISCONNECTED, EV_TIMEOUT } ble_ev_t;

uint32_t ble_next(uint32_t s, ble_ev_t ev)
{
    if (ev == EV_DISCONNECTED && s >= BLE_CONNECTING && s <= BLE_RECEIVING)
        return BLE_IDLE;
    switch (s) {
    case BLE_STARTING:
        return ev == EV_INIT_OK ? BLE_IDLE : (ev == EV_INIT_FAIL || ev == EV_TIMEOUT) ? BLE_FAILED : s;
    case BLE_SCANNING:
        return ev == EV_SCAN_DONE ? BLE_IDLE : s;
    case BLE_CONNECTING:
        return ev == EV_CONNECTED ? BLE_DISCOVERING : (ev == EV_CONN_FAIL || ev == EV_TIMEOUT) ? BLE_IDLE : s;
    case BLE_DISCOVERING:
        return ev == EV_FOUND_MIDI ? BLE_SUBSCRIBED : (ev == EV_NO_MIDI || ev == EV_TIMEOUT) ? BLE_IDLE : s;
    case BLE_SUBSCRIBED:
    case BLE_RECEIVING:
        return ev == EV_NOTIFY ? BLE_RECEIVING : s;
    default:
        return s;
    }
}

#ifndef BLE_CENTRAL_STATE_ONLY
/* ... the glue below ... */
#endif
```

Run step 2's command again: all `ok`. Then write the glue under `#ifndef BLE_CENTRAL_STATE_ONLY`.
- Include the SDK headers through `ble_sdk.h`, a small header in `firmware/src/ble/`. It declares only the SDK
  functions, structs and constants listed in **Interfaces**, copied with their exact definitions from
  `include_lib/btstack/le/ble_api.h` and `att.h`, with a comment naming the line of each.
- The glue's parts:
  - **Globals:**
    - `static uint32_t st;`
    - `static ble_scan_t scan;`
    - `static ble_midi_t bm;`
    - `static uint16_t con_handle, midi_handle;`
    - `static uint32_t deadline_ms;`
    - `static uint8_t att_ram[512] __attribute__((aligned(4)));`
    - `static uint8_t search_ram[256] __attribute__((aligned(4)));`
    - a 64-entry MIDI ring of `uint32_t` (head and tail counters, `RING_PUBLISH()` as in `usb.c`);
    - a monitor array of the last 10 packets.
  - **`ble_start()`**, only from `BLE_OFF`:
    1. `ble_os_init(ble_heap, sizeof ble_heap)`;
    2. `ble_on = 1`;
    3. `st = BLE_STARTING`, deadline 5 s;
    4. `btstack_init()`.

    When `BT_STATUS_INIT_OK` arrives through the event hook (`bt_event_notify` in `ble_port.c`, which calls
    `ble_event(BT_STATUS_INIT_OK)`), run the profile init from `le_net_central.c` 1308–1330, but central only:

    ```c
    le_device_db_init();
    ble_stack_gatt_role(1);
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(0);
    sm_set_encryption_key_size_range(7, 16);
    sm_set_master_request_pair(0);
    gatt_client_init();
    gatt_client_register_packet_handler(cbk_packet_handler);
    hci_event_callback_set(cbk_packet_handler);
    le_l2cap_register_packet_handler(cbk_packet_handler);
    ```

    Then `st = ble_next(st, EV_INIT_OK)`.
  - **`ble_scan()`**, from IDLE:
    1. `ble_scan_clear(&scan)`;
    2. `ble_op_set_scan_param(1 /* active */, 48 /* 30 ms */, 48)`;
    3. `ble_op_scan_enable2(1, 0)`;
    4. `st = BLE_SCANNING`, deadline 10 s.

    In `ble_tick()` (called from `ble_service()` after the OS), at the deadline: `ble_op_scan_enable2(0, 0)`,
    `st = ble_next(st, EV_SCAN_DONE)`.
  - **`cbk_packet_handler(packet_type, channel, packet, size)`**, following `le_net_central.c` 853–960. Its cases:
    - **`GAP_EVENT_ADVERTISING_REPORT`:** `adv_report_t *r = (void *)&packet[2];` then
      `ble_scan_report(&scan, r->address, r->address_type, r->rssi, r->data, r->length)`.
    - **`HCI_EVENT_LE_META` / `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`:** the status is `packet[3]`. On success
      (status 0): `con_handle = little_endian_read_16(packet, 4)`, then
      `ble_op_att_send_init(con_handle, att_ram, sizeof att_ram, 64)`, then
      `user_client_init(con_handle, search_ram, sizeof search_ram)`, then
      `ble_op_search_profile_uuid128(MIDI_SVC_UUID_BE)`, then `EV_CONNECTED`, deadline 10 s. On failure:
      `EV_CONN_FAIL`.
    - **`HCI_EVENT_DISCONNECTION_COMPLETE`:** record the reason `packet[5]`; `con_handle = 0`;
      `ble_op_att_send_init(0, 0, 0, 0)`; `EV_DISCONNECTED`.
  - **`user_client_report_search_result(search_result_t *r)`:**
    - `r == 0` means the search ended. If `midi_handle` was found, write the CCCD:

      ```c
      static const uint8_t on[2] = {1, 0};
      ble_op_att_send_data(midi_handle + 1, on, 2, ATT_OP_WRITE);
      st = ble_next(st, EV_FOUND_MIDI);
      ```

      Otherwise `ble_op_disconnect(con_handle)` and `EV_NO_MIDI`.
    - For a real result whose `characteristic.uuid16 == 0` and whose `characteristic.uuid128` equals the MIDI
      characteristic UUID (`7772E5DB-...`, compared in the byte order the SDK reports; check the order once on the
      device, step D), set `midi_handle = r->characteristic.value_handle`.
  - **`user_client_report_data_callback(att_data_report_t *d)`:** if
    `d->packet_type == GATT_EVENT_NOTIFICATION && d->value_handle == midi_handle`, then
    `ble_midi_parse(&bm, d->blob, d->blob_length, ring_put, 0)` and `st = ble_next(st, EV_NOTIFY)`.
  - **`ble_connect(i)`**, from IDLE with `i <` the sorted count: `struct create_conn_param_t c` with
    `conn_interval = 12` (15 ms), `conn_latency = 0`, `supervision_timeout = 400` (4 s), the device's address and
    type. Then `ble_op_create_connection(&c)`, `st = BLE_CONNECTING`, deadline 10 s. At the deadline:
    `ble_op_create_connection_cancel()` and `EV_TIMEOUT`.
  - **`ble_disconnect()`:** `if (con_handle) ble_op_disconnect(con_handle);`
  - **`ble_midi_take(uint32_t *pkt)`:** pops one packet from the ring (returns 0 when empty). The main loop pushes
    each one into the monitor list and, in step 5, into `midi_out_event(pkt)`.
- Steps gate the behaviour with `#if FELUCCA_BLE_STEP >= N`:
  - step 2 enables `ble_start` + `ble_scan`, and the screen shows the report counter;
  - step 3 enables the list;
  - step 4 enables connect / discover / subscribe;
  - step 5 enables the monitor and USB forwarding.

Include order in `ble.c` (Task 8 replaces its tail): `ble_central.c` and `ble_ui.c` come **after** the
definitions of `ble_heap` and `ble_on`, and `ble_service()` becomes:

```c
void ble_service(void)
{
    if (!ble_on)
        return;
    ble_os_service(50000);                         /* at most ~50 ms per main-loop pass */
    ble_tick();                                    /* deadlines: scan end, connect / discovery timeouts */
}
```

`ble_tick()` (in `ble_central.c`) checks `deadline_ms` against `fm1_ms` for the current state and applies the
timeout transitions: scan end gives `EV_SCAN_DONE`; STARTING / CONNECTING / DISCOVERING give `EV_TIMEOUT`, with
`ble_op_create_connection_cancel()` or `ble_op_disconnect()` as the table says.

- [ ] **Step 4: Write `ble_ui.c` (screens and keys)**

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
static uint32_t ui_sel;                            /* index into the sorted list */
static uint32_t ui_dirty = 1;

void ble_key(uint32_t pressed, int32_t select_steps)   /* the only entry from the core (gate 4 ALLOWED) */
{
    uint32_t play = pressed & (1u << panel.btn[B_PLAY]), rec = pressed & (1u << panel.btn[B_REC]);
    uint8_t idx[BLE_SCAN_MAX];
    uint32_t n = ble_scan_sorted(&scan, idx);
    if (select_steps) {
        int32_t s = (int32_t)ui_sel + select_steps;
        ui_sel = n ? (uint32_t)(s < 0 ? 0 : s >= (int32_t)n ? (int32_t)n - 1 : s) : 0;
        ui_dirty = 1;
    }
    if (play) {
        if (st == BLE_OFF)
            ble_start();
        else if (st == BLE_IDLE)
            ble_scan();
        ui_dirty = 1;
    }
#if FELUCCA_BLE_STEP >= 4
    if (rec) {
        if (st == BLE_IDLE && n)
            ble_connect(ui_sel);
        else if (st >= BLE_CONNECTING && st <= BLE_RECEIVING)
            ble_disconnect();
        ui_dirty = 1;
    }
#else
    (void)rec;
#endif
}
```

`ble_draw()` draws when `ui_dirty`, the state changed, or the counters changed (at most every 100 ms). It uses
`lcd_fill` / `draw_text_line(x, y, w, &AF_S, text, T_TEXT, T_BG, 0)` as `core_main.c` does:
- **Line 0:** `BLE MIDI TEST` and the state name.
- **Line 1:** `REPORTS n` in step 2 and later; the error text in FAILED.
- **Lines 2–17, step 3 and later:** the sorted list. Each row is name-or-address, RSSI and `MIDI`; the selected row
  is shown inverted (`T_INK` on `T_SEL`).
- **Connected states, step 4 and later:** the device name, the state, `MSG n  PKT n  ERR n`, and the last 10
  monitor lines.

Decode a monitor packet into text like this (`cin` = low nibble):

| `cin` | Text |
| --- | --- |
| 9 (note on, velocity > 0) | `NOTE ON ch NOTE vVEL` |
| 8, or 9 with velocity 0 | `NOTE OFF ch NOTE` |
| B | `CC ch #NN = VV` |
| C | `PC ch NN` |
| E | `PB ch +/-N` (centred on 8192) |
| 4–7 | `SYSEX` (once per completed SysEx, with the byte count) |
| F | `RT XX` |
| anything else | `MIDI XX XX XX` |

Note names: `C`, `C#`, `D` … with the octave as `note / 12 - 1`.

In `core_main.c`'s loop, replace the BLE block from Task 7 with:

```c
#if FELUCCA_BLE
        {
            static uint32_t prev;
            uint32_t now = fm1_in.buttons, down = now & ~prev;
            prev = now;
            ble_key(down, panel_enc(EN_SELECT));
        }
        ble_service();
        ble_draw();
#endif
```

and add `ble_status()` as the `ble` console command in `console.c` (next to `status`).

- [ ] **Step 5: Gates on each step build, then commit**

For each step N = 2, 3, 4, 5 run:

```bash
FELUCCA_BLE=1 FELUCCA_BLE_STEP=$N PYTHON=$VENV/bin/python ./build.sh 2>&1 | tail -4
shasum -a 256 build/loader/ota.bin
python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref
python3 tools/check_ble_boot.py build/felucca.dis
tests/run_tests.sh 2>&1 | tail -2
```

Every gate must pass for every N.

```bash
git add firmware/src/ble firmware/src/core_main.c firmware/src/console.c tests/ble/ble_central_test.c tests/run_tests.sh
git commit -m "ble: central (scan, connect, BLE-MIDI subscribe), screens and keys, staged by FELUCCA_BLE_STEP"
```

---

### Task 9: Device step A, B, C, D, E (one build each; the user installs)

**Files:**
- Create: `docs/ble/DEVICE_STEPS.md` (results per step, filled in as they come)

Per step, Claude does all of this and nothing on the device:

- [ ] **Step 1: Build and gate the step's package**

```bash
FELUCCA_BLE=1 FELUCCA_BLE_STEP=$N PYTHON=$VENV/bin/python ./build.sh
shasum -a 256 build/loader/ota.bin       # pinned hash
python3 tools/check_update_path.py build/felucca.dis tests/ble/update_path.ref
python3 tools/check_ble_boot.py build/felucca.dis
tests/run_tests.sh 2>&1 | tail -2        # ALL HOST TESTS PASSED
cp build/felucca.fwsc build/ble-step$N.fwsc
python3 web/make_site.py build/ble-step$N.fwsc ble-step$N build/site
```

Then serve: `python3 -m http.server 8771 --bind 127.0.0.1 -d build/site` (in the background).

- [ ] **Step 2: Give the user the link and the checklist for the step** (from the spec table)

| Step | `N` | What the user does |
| --- | --- | --- |
| A | 1 | Install; check the home screen; then install B from it through the installer (that proves A's update entry) |
| B | 2 | Press PLAY with the SMC-Mixer on; read the `REPORTS` count; console `ble` |
| C | 3 | Find the SMC-Mixer with `MIDI` in the list |
| D | 4 | Select it, press REC; the state reaches `SUBSCRIBED` |
| E | 5 | Move a fader; CCs on screen and in a Mac MIDI monitor (for example MIDI Monitor by Snoize) |

The hold reminder for every step: OCT- + OCT+ held 5 s gives UBOOT immediately, with no countdown in this core.

- [ ] **Step 3: Read the results over the console (`ble`, `status`) and record them in `docs/ble/DEVICE_STEPS.md`**
- [ ] **Step 4: On a failure, use superpowers:systematic-debugging.** Add only diagnostics to the next build, and
  never start BT at boot. Commit the notes.
- [ ] **Step 5: Commit after each step**

```bash
git add docs/ble/DEVICE_STEPS.md
git commit -m "ble: device step $N results"
```
