# BLE MIDI test firmware for the FM-1: design

Date: 2026-10-08. Author: DEADACTIVE (with Claude). Branch: `ble` (from Felucca 1.0.5.2, `main`).

## Goal

A standalone test firmware that proves the FM-1 can be a BLE-MIDI **central**:

- scan for Bluetooth LE devices and list them;
- connect to a BLE-MIDI device (the test device is an M-VAVE SMC-Mixer);
- show the incoming MIDI on screen;
- forward it to USB MIDI out.

It is built on the stable stripped Felucca core: `-Os`, no experimental flags, no overclocking. The Bluetooth part is
a self-contained module with a small interface, so it can later move into Hortator.

Not in this version:

- pairing or bonding;
- more than one connection;
- remembering devices or auto-connecting;
- MIDI out to the BLE device;
- any synth sound;
- Hortator integration;
- clock changes.

## Hard rule: the updater stays reachable

The 2026-10-08 incident: a probe build compiled at `-O2` broke its own USB update entry, and the FM-1 had to be
restored through the mask-ROM boot mode. Every build of this firmware keeps the ways back:

- **The app's update entry.** Web installer and M-UPGRADE reach it through the USB-MIDI SysEx. Its code (USB,
  `ota.c`, the loader) is the stripped core's, at `-Os`, and its machine code is checked byte-for-byte (gate 2 below).
- **OCT- + OCT+ held 5 s:** enters UBOOT from the main loop.
- **The boot guard:** two crashes or watchdog resets in the first 30 s enter UBOOT.
- **The Mac recovery tool:** Hortator `docs/RECOVERY.md` / `tools/rescue/fm1_uboot.py`, which writes through UBOOT.
- **Nothing Bluetooth runs at boot.** The stack starts only on a key press from the main loop (gate 4), so a BT crash
  can never repeat on every power-on.

## Background (facts this design rests on)

- **The radio.** The FM-1 has a wire antenna on the SoC's RF pad (aroum/fm1-custom-fw board photos). The stock
  firmware is a BLE-MIDI GATT server built on JieLi's controller (`btctrler`) and a btstack-derived host
  (AL-255/FM-1-RE `docs/io/10-bluetooth.md`). Stock never scans or connects as central.
- **The JieLi AC79 SDK ships a working BLE central / GATT client.** It does scanning (`ble_op_set_scan_param`,
  `ble_op_scan_enable2`, `GAP_EVENT_ADVERTISING_REPORT`), connecting (`ble_op_create_connection`), discovery
  (`ble_op_search_profile_uuid128`), CCCD writes (`ble_op_att_send_data`) and notifications
  (`user_client_report_data_callback`). Demo: `apps/common/ble/le_net_central.c`.
  - A `demo_ble` central build without SDRAM links at about 282 KB of text.
  - The BT stack itself costs about 92 KB of code (controller 49 KB, host 43 KB) and about 8.2 KB of static RAM.
  - The SDK has no BLE-MIDI code.
- **The libraries.** They ship as LLVM 4.0.1 bitcode only: `cpu/wl82/liba/btctrler.a` and `btstack.a`, plus radio
  setup from `wl_rf_common.a` and `cpu.a`. They run as two tasks, `"btctrler"` and `"btstack"`, that block forever
  in `os_taskq_pend` and `os_sem_pend`.
  - They need: OS calls, `sys_timer` / `sys_timeout`, `lbuf` / `cbuf`, `malloc` (the controller takes 3072 + 512),
    `request_irq` for IRQ 29 (`IRQ_BLE_RX_IDX`) and IRQ 45 (`IRQ_BLE_EVENT_IDX`) at priority 2,
    `local_irq_*`, `low_power_*`, `clk_get`, and `syscfg_read`.
  - Config IDs read: 102 BT MAC, 110 BT frequency-offset trim, 197 FCC adjust.
  - The undefined-symbol list (389 entries, most of them `config_*` constants and log tags) was taken from the
    V1.2.0 libraries.
- **Flash areas the BT code must never write:** VM `0x94000`, BTIF `0xE9000`, key_mac `0xFF000`. A full backup of
  the test FM-1 exists: `~/Desktop/fm1_backup_20261008.bin`.

## Architecture

```
core main loop (unchanged)            ble/ module                      JieLi libs (bitcode)
  usb/ota/cdc/ui  ──ble_service()──►   ble_os: cooperative tasks  ◄──►  btctrler task, btstack task
  PLAY/REC/SELECT ──ble_start() etc.   ble_central: scan/connect/GATT   (IRQ 29/45 → controller ISRs)
  screen, USB MIDI out ◄── MIDI ring ◄─ ble_midi: BLE-MIDI → messages
```

### Unchanged core

The stripped core from the parked LSDj work (`lsdj` branch, commit `407b0e7`), without the probe code and the
probe build hooks:

- boot and the boot guard;
- `usb.c` (MIDI, CDC console, UAC, the update SysEx), `ota.c` / `ota_hw.c` and the loader;
- the flash driver and `storage.c`;
- `lcd.c` / `gfx.c` with the fonts;
- the input matrix and `panel.c`;
- the audio ISR, rendering silence.

All of it is built at `-Os`, as now.

### `firmware/src/ble/`

| File | Responsibility |
| --- | --- |
| `ble_os.c` | What the JieLi libraries call: `task_create` (a context with its own stack from POOL), `os_taskq_pend` / `os_taskq_post_type`, `os_sem_*`, `os_mutex_*`, `os_time_dly`, `os_current_task`, `sys_timer_*` / `sys_timeout_*` / `usr_timeout_*`, `malloc` / `zalloc` / `free` (a fixed POOL heap, first fit, with overrun guards), `request_irq` (IRQ 29/45 through the HAL's vector table), `local_irq_*` (`cli` / `sti`), `syscfg_read` (MAC and trims: defaults or read-only flash; never writes), and stubs (`low_power_*`, `clk_get`, `printf`, the classic / TWS / LE-audio / LwIP hooks). It also defines the `config_*` constants for a BLE-central-only stack (`config_btctler_le_roles = LE_SCAN \| LE_INIT \| LE_MASTER`, one link). Each stub is listed and justified. |
| `hal/fm1_ctx.S` | Context switch: save callee-saved registers, `sp` and the return address; switch stacks. In `hal/`, as the build's register-access check requires. |
| `ble_central.c` | The app on the SDK's `ble_op_*` API. Start the stack, set scan parameters, scan for 10 s, keep the results table, connect, search the MIDI service by UUID128 `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`, find characteristic `7772E5DB-3868-4112-A1A9-F2669D106BF3`, write its CCCD (notify), receive notifications. 10 s timeouts. Error codes. |
| `ble_midi.c` | BLE-MIDI packet parser (Apple / MMA BLE-MIDI). Handles the header byte, the per-message timestamp bytes, running status, several messages per packet, real-time bytes and SysEx across packets. Output: whole MIDI messages, as USB-MIDI 4-byte events, into a 64-entry ring. Malformed input is counted, never fatal. Pure C, no dependencies: host-testable. |
| `ble_ui.c` | The scan screen and the connected screen (behaviour below). |

**Interface to the core:**

- `ble_start()` and `ble_scan()`, on PLAY;
- `ble_connect(i)` and `ble_disconnect()`, on REC;
- `ble_service()`, once per main-loop pass;
- read-only accessors for the state, the device table, the counters and the last error;
- `ble_midi_take()`, which drains the MIDI ring.

### Threading model

The JieLi tasks are cooperative contexts.

- `ble_service()` switches to each ready task and returns when all of them block. Ready means a queue message, a
  semaphore, an expired delay or an expired timer.
- One pass is bounded: it returns after at most about 50 ms, so the watchdog (8 s) and the UI keep running.
- The core is never preempted by a BT task.
- The radio IRQs (29, 45) run the controller's ISRs, which post to the task queues. The tasks run on the next
  `ble_service()`. The main loop's 15 ms frame already polls often; a tighter loop runs while BT is active, if
  scanning or the link needs it (measured in step B).

### Data flow

1. The radio IRQ runs the controller ISR.
2. The controller task and the host task run inside `ble_service()`.
3. The GATT notification goes to `ble_midi_parse()`, then to the MIDI ring.
4. The main loop drains the ring into the monitor list on screen and into the core's USB MIDI out ring
   (`midi_out_event`), which `usb_poll` sends.

### Priorities and timing

- The BT IRQs keep the SDK's priority 2, below audio (3) and TIMER5 (4). Here audio renders silence, so it is short.
- Risk for Hortator later: a long synth render delays the radio's IRQs. It is not solved here; step B measures the
  BT IRQ latency to have the number.

### Linking

- `build.py` gains a `FELUCCA_BLE=1` build that adds the BT bitcode libraries and the radio-setup objects. They come
  from the SDK checkout (`AC79_SDK`), with the `liba` files pinned by SHA-256 as `SDK_SHA256` already pins the SDK
  files.
  - The local V1.2.1 checkout is sparse and has no `cpu/wl82/liba/`: its blobs get fetched. The V1.2.0 copy in
    `~/Downloads` is the reference.
- `felucca.c` stays a native `-Os` object, so LTO optimises only the library bitcode. Gate 2 verifies that the core
  did not change.
- The default build (`FELUCCA_BLE` unset) is the stripped core, byte-identical to the reference build.

## Behaviour (approved)

**Boot.** As the stripped core: the boot guard and the UBOOT key first. Home screen: `BLE MIDI TEST` and the
version. Bluetooth is off until a key.

**Controls.**

| Control | Action |
| --- | --- |
| PLAY | Start Bluetooth (first press), then scan for 10 s; again: rescan |
| SELECT knob | Move through the device list |
| REC | Connect to the selected device; when connected: disconnect |
| OCT- + OCT+ held 5 s | UBOOT (unchanged) |

**Scan screen.**

- Up to 16 devices, strongest signal first.
- Each row: the name (from the complete or shortened local name in the advertising or scan-response data; else the
  address), the RSSI in dBm, and **MIDI** if the 128-bit service list holds the BLE-MIDI UUID.
- Status line: the scan state and the number of advertising reports.

**Connected screen.**

- The device name.
- The state: connecting → discovering → subscribed → receiving.
- A MIDI monitor of the last 10 messages, decoded: `NOTE ON 1 C4 v100`, `CC 1 #07 = 64`, `PB 1 +512`,
  `SYSEX 12 B`, ...
- Counters: messages, BLE packets, parse errors.
- When the link drops: `DISCONNECTED (reason)`, then back to the list.

**USB.** Every received MIDI message also goes out on USB MIDI to the computer, cable 0.

**Console.** `ble` prints the state, the counters, the scan table and the last error. The console stays read-only.

**Errors.**

- Every failure (init, scan, connect, discovery, subscription) shows its reason and code on screen and in `ble`.
- Connect and discovery each time out after 10 s.
- If the stack cannot start: `BT FAILED (code)`. The rest of the firmware keeps working.

## Verification gates (a build fails if any fails)

1. **Loader byte-identical.** `build/loader/ota.bin` sha256 = `e671adf0ce6e4f69ac8514087c8833b1969fa4c470645786f19968e90b9a630f`. This becomes a hard check in `build.py`,
   for every build.
2. **Update path unchanged.** A script compares, between this build and the stripped-core reference build, the
   machine code of:
   - `usb_*`, `ep0_*` / `ep1_*` and `sysex_byte`;
   - `ota_*`;
   - `fm1_timer5_irq`;
   - `fm1_cstart` / `fm1_main`'s boot path up to the main loop;
   - the flash driver (`.ram_text`).

   They must be byte-identical, apart from address operands that are relocations. The relocation rule is: compare
   after normalising call and load targets to symbol names.
3. **Host tests pass.**
   - The existing loader / OTA / install tests.
   - `ble_midi` parser tests: single messages, running status, several per packet, timestamp wrap, SysEx across
     packets, malformed packets.
   - `ble_os` tests on the host: queue order, semaphore wake-up, timer expiry, heap bounds, context switching with a
     portable host back end.
   - Scan table tests: dedup by address, RSSI order, the cap of 16, name parsing from AD structures, the MIDI tag.
4. **No BT before the key.** A static check over the call graph in `felucca.dis`: no `ble_*` init or `btstack_init`
   is reachable from `fm1_cstart` / `fm1_main` outside the main loop's key handler.

## Device steps (each a separate build, installed by the user)

Each build is installed with the web installer from the previous one. That proves the update entry of every build.

| Step | Build | Pass |
| --- | --- | --- |
| A | BT linked in, never started | Boots, works as the core; installing B from it works |
| B | PLAY starts the stack and a 10 s scan | The advertising-report counter is above 0 with the SMC-Mixer on; BT IRQ latency is recorded |
| C | Device list | The SMC-Mixer appears with **MIDI** |
| D | Connect, discover, subscribe | The state reaches `subscribed` |
| E | MIDI monitor and USB forwarding | A fader moved on the SMC-Mixer shows CCs on screen and in a Mac MIDI monitor |

If a step fails, the next build adds diagnostics. It never adds wider changes. Fixing a failure follows
systematic debugging: root cause first.

## Risks and open questions

- **Radio setup may depend on SDK state** (clock tree, power and LDO init, cache-way switching, P33). Mitigation: step
  B is the first contact. `ble_os` logs every library call into a RAM trace that `ble` prints. Where the radio needs
  setup the core lacks, the SDK's `setup.c` / `power_init` order is the reference.
- **RF trim and MAC.** Without config ID 110 the controller uses default trims, which may give weaker range. The MAC
  comes from config ID 102 if a read-only source has it (BTIF / VM written by stock), else a random static address derived
  from the flash chip's unique ID (the read-only 0x4B command, as the SDK does), so it is unique and stable per FM-1.
- **Bitcode libraries can't be debugged at source level.** Mitigation: the RAM trace and the console.
- **The SMC-Mixer may require pairing.** If step D fails with an insufficient-authentication error, add Just Works
  pairing with no bonding storage (still no flash writes). That is a separate design change.
- **IRQ latency with a synth (Hortator later).** Out of scope; measured in step B for later.
- **Licence.** The SDK is Apache-2.0 (`LICENSES/Apache-2.0.txt` already ships with packages). The bitcode host is
  BTstack-derived, and JieLi distributes it under its SDK licence. This is noted in `LICENSING.md` when BLE ships.
