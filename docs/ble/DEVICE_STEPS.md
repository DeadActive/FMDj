# BLE MIDI test: device steps

Each step is one package, installed by the user from the previous one through the Felucca web installer. Every
package passed, before install:
- the loader hash (e671adf0…);
- gate 2a/2b (update path unchanged);
- gate 4 (no BT start-up at boot);
- the host tests.

The way back in every step: OCT- + OCT+ held 5 s gives UBOOT at once (no countdown). If the app does not start,
use the UBOOT recovery (Hortator `docs/RECOVERY.md`).

| Step | Build | Package sha256 (first 16) | What to check | Result |
| --- | --- | --- | --- | --- |
| A | `FELUCCA_BLE_STEP=1` (linked, never started) | a403bb5818f4c8fe | home screen; then install B from it | ok: installed, B installed from it (first round) |
| B | `FELUCCA_BLE_STEP=2` (start, scan) | 9d31c2057b5b41e7 | PLAY: `STARTING` → `IDLE`; PLAY again: `REPORTS` counts up; console `ble` | pending |
| C | `FELUCCA_BLE_STEP=3` (list) | a3d7c6cfcc07b3ab | the SMC-Mixer in the list with `MIDI` | pending |
| D | `FELUCCA_BLE_STEP=4` (connect) | e5b178bfb30c310f | SELECT it, REC: the state reaches `SUBSCRIBED` | pending |
| E | `FELUCCA_BLE_STEP=5` (monitor, USB) | 99c90e3a58259045 | move a fader: `CC` lines on screen and in a Mac MIDI monitor | pending |

## Results

### Round 1 (packages of a4781e3)

- A: installed; the update entry worked (B, C, D installed through the installer).
- B, C, D: **the app crashed on PLAY.** The crash record was lost to a power cycle (`crash`: no record; `boots 1`).

**Cause, from the code.** Two of the core's hardware guards, which the host tests cannot see:
1. **The EMU stack limit** allows `sp` only in `[_ustack_lo, _sstack_top]` (0x1C74100..0x1C7C000). The BT tasks'
   stacks are in `.pool` (0x1C20000..). The first switch into a BT task raises the CPU exception, which shows CRASH
   and reboots after 4 s.
2. **The top-of-RAM write lock** (`fm1_guard_lock_top`, 0x1C7FD50..0x1C7FFFF) covers the vector table (0x1C7FE00).
   The stack's `request_irq` writes vectors after boot, which is a write-limit fault.

**Fix (round 2).**
- `ble_start` widens the stack window to `[_pool_start, _sstack_top]` (`fm1_guard_stack_window`, `core_ble_stack_window`).
- `core_irq_attach` reopens the top lock around the vector write; `request_irq` makes the attach with IRQs off.

**Diagnostics added.**
- A `.noinit` breadcrumb: how far `ble_start` got, and the BT task running, both kept over a crash reboot.
- The stack high-water mark of each task.
- The IRQs the stack requested.

All of these show in the console's `ble`.

### Round 2 (packages of the fix)

- **B (9d31c205…): ok.**
  - PLAY: STARTING → IDLE, no crash.
  - Console `ble`:
    - `stage 7`: BT_STATUS_INIT_OK reached.
    - IRQs 29 and 45 requested. The slot timer, 41, is not used in BLE-only mode.
    - Stack high-water: btctrler 784 B and btstack 1356 B of 4096.
  - One 10 s scan: 605 reports and 16 devices (the table's limit), RSSI -53 to -91 dBm. Mostly random addresses
    (t1), with no name in the advertisement. None tagged MIDI: the SMC-Mixer was not recognised in this scan
    (still to check: was it on and not connected to another host?).
- **C (a3d7c6cf…): ok.** With the mixer on and not connected to any other host, the scan finds
  `5E8BA0792810 t0 -64 MIDI SMC-Mixer`: a public address, the name and the BLE-MIDI service UUID all read. The B
  round's missing mixer was the mixer, not the list.
- **D (e5b178bf…): PLAY crashed about half the time** (red screen, reboot); when it started, the scan worked.
  Crash record: vec 1, pc `0x470`, rets `0x207A2C4` (in `rd_adc_peak` after `call delay_us`), emu 0. Breadcrumb:
  stage 5, task btctrler.

  **Cause.** The radio calibration ran from XIP. The SDK runs it from RAM with flash access stopped, because
  reading the flash during the ~500 ms trim can hang it. That gave garbage fetches: our `delay_us`, the trim code
  itself, `memcpy`, and the core's interrupt handlers all ran from XIP.

  **Fix (round 3):** see `LINK_NOTES.md`, "The radio calibration runs from RAM".
