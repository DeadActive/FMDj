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
- **D round 3 (3961ea50…, calibration in RAM): started once, then crashed on PLAY in most tries again.** The
  device was off USB, so there was no crash record.

  **Next D (ff66d9a6…) adds diagnostics only:**
  - `ble` prints the whole crash record (dbg, icfg and the four-entry branch trace too);
  - a `trim` breadcrumb: 1 inside the radio calibration, 2 after it.
- **D (ff66d9a6…, diagnostics): PLAY reset the device.**
  - `crash`: no record. `prev_rst 0x04`: a **watchdog** reset.
  - Breadcrumb: stage 5, task btctrler, **trim 1**. The CPU stopped inside the radio calibration, with
    interrupts off, until the 8 s watchdog fired.

  **Two candidates:**
  1. Our `delay_us` waited on TIMER4 (crystal-clocked) while the calibration retunes the crystal and the clocks.
     The SDK's `delay_us` is a counted loop with no timer (cpu.a `delay.c`). The round-2 crash was also inside
     `delay_us`.
  2. The SDK also quiets the flash for the trim (`norflash_enter_spi_code`: SFC off, flash out of continuous-read
     mode).

  **Next D (42f6b48c…):**
  - `delay_us` / `hw_udelay` are counted RAM loops, as in the SDK, calibrated against TIMER4 at start-up
    (rounded up).
  - Diagnostics: the delay count, the last delay's length, and `t4_stalls` (delays during which TIMER4 did not
    move).
- **D (42f6b48c…, counted delays): a loud tone that changed pitch, then a watchdog reset.**
  - The tone: with interrupts off, the audio DMA repeats its buffer, and the trim retunes the clocks under it.
  - `t4_stalls 0`: TIMER4 did not stop, so candidate 1 is ruled out.
  - The last delay was 80 µs: wf_rf_common_init's `set_pll_rn_en(); delay_us(80)` just before `wf_rf_trim`.
  - The trim hung before `rd_adc_peak` (its only delay_us), in `wf_rx_tune_test`'s poll of an RF "done" bit.

  **The way round it, as the stock firmware does.** The library skips the trim when `RFIinitUseTrimValue` is set
  and `wifi_get_rf_trim_data` returns a valid stored record (`wifi_trim_is_already` / `quick_start_wifi_trim`).
  The stock firmware stored one on this FM-1:
  - **Where:** flash 0xE8019, in its config store (sector 0xE8000, `55 AA AA 55`).
  - **Item format:** `[CRC16 low byte][id low byte][len << 4 | id >> 8][data]`.
  - **The neighbouring items:** id 106 crystal trim `0B 0B`, id 107 PA trim `01 07 04 07 0B 01 07` (both what the
    port already used), and id 187 = VM_WIFI_RF_INIT_INFO, 68 bytes with its own CRC16.

  **Next D (0b04b0a6…):** at BLE start, scans 0xE5000..0xFC000 (read only, through the core's `st_read`) for the
  last valid record and hands it to the library, so the trim does not run. With no record, BT does not start:
  FAILED with `NO RF TRIM IN FLASH`.
- **D (0b04b0a6…): ok.** BT starts every time, right after boot, with no wait. The scan finds the SMC-Mixer; REC
  connects; the state reaches RECEIVING, and messages count up as the knobs move.
  - Console `ble`:
    - `state RECEIVING msgs 294 pkts 293 errs 0 lost 0`;
    - `rf trim stored` (the trim never ran: `trim 0`);
    - stage 7;
    - stack high-water: btctrler 1104 B, btstack 1492 B of 4096;
    - counted delay loop: 52 loops/us.
- **E (e6a8d3f7…): ok.** The connection and the on-screen MIDI monitor show the mixer's values.
  - Console `ble`: `state RECEIVING msgs 2728 pkts 1798 errs 0 lost 0`.
  - Core `status`: `midi_tx_pkts 448`. These are USB IN transfers, each carrying up to 16 events, not a count of
    MIDI events.
  - Still to confirm on the Mac side: the CCs reaching a MIDI monitor app.

## Summary

The FM-1 scans for BLE devices, lists them, connects to a BLE-MIDI device (M-VAVE SMC-Mixer), subscribes to its
MIDI characteristic, shows the incoming MIDI on screen and forwards it to USB MIDI. It runs on the stripped core
with no overclock and no experimental flags. Bluetooth starts only on a key press. The update path is unchanged
in every build, and the loader is pinned.

What it took on the device, beyond the plan:
1. The core's stack-limit and vector-table guards had to allow the BT task stacks and `request_irq`.
2. The radio calibration (`wf_rf_trim`) cannot run on this core's clock and flash setup: it crashed when run from
   XIP and hung when run from RAM with interrupts off. The stock firmware's stored calibration (config store item
   187) is used instead, as the SDK provides for (`RFIinitUseTrimValue`).
