# BLE link notes (JieLi AC79 SDK V1.2.0 libraries)

These facts were read from the SDK's prebuilt bitcode (`cpu/wl82/liba/*.a`) with Homebrew `llvm@17`:

```
llvm-ar x system.a os_api.c.o
llvm-dis --disable-auto-upgrade-debug-info os_api.c.o
```

The JieLi BT libraries call these functions. `firmware/src/ble/ble_os.c` must behave the same way.

## Sizes and codes (`tools/ble_libs.py abi`, from the SDK headers with demo_ble's flags)

- `OS_SEM`, `OS_MUTEX` and `OS_QUEUE` are each 80 bytes: FreeRTOS `StaticSemaphore_t` / `StaticQueue_t`. The
  libraries embed them in their own structures, so ours must fit inside 80 bytes.
- Return codes:

  | Code | Value |
  | --- | --- |
  | `OS_NO_ERR` | 0 |
  | `OS_TIMEOUT` | 11 |
  | `OS_TASKQ` | 13 |
  | `OS_TASK_NOT_EXIST` | 14 |
  | `OS_Q_FULL` | 21 |
  | `OS_Q_EMPTY` | 22 |

## Task queues (`os_api.c`)

**`__os_taskq_post(name, type, argc, argv)`**, which `os_taskq_post_type` calls:
- caps `argc` at 18;
- puts a head word `(type << 8) | (argc & 0xFF)` into the queue, then the `argc` argument words;
- returns:

  | Result | Case |
  | --- | --- |
  | 0 | posted |
  | 14 | no such task |
  | 4 | the task has no queue |
  | 21 | not enough room for head + args |

  It never blocks.

**`os_taskq_pend(fmt, argv, argc)`** is `__os_taskq_pend(argv, -, -1)`: it waits forever. `fmt` and `argc` are
ignored.
- If the current task has no queue, it does `os_time_dly(2)` and retries.
- It takes one message:
  - `argv[0] = head >> 8`, the type word: `Q_MSG` 0x100000, `Q_EVENT` 0x200000, `Q_CALLBACK` 0x300000 or
    `Q_USER` 0x400000, OR-ed with a message code;
  - `argv[1..n] =` the `n = head & 0xFF` argument words.
- **`Q_CALLBACK` (`(argv[0] & 0xF00000) == 0x300000`) is handled inside the pend and never returned:**
  - `argv[1]` is the function, `argv[2]` the flags word `F`, and the arguments start at `argv[3]`;
  - the call shape depends on `F & 0xFF`:

    | `F & 0xFF` | Call |
    | --- | --- |
    | 1 | `func(argv[3])` |
    | 2 | `func(argv[3], argv[4])` (when `F & 0x400` is clear) |
    | otherwise | `func(argv[3], (F & 0xFF) - 1, &argv[4])` |

  - **`F & 0x100`:** store the return value through the pointer that follows the arguments;
  - **`F & 0x200`:** `os_sem_post` the semaphore that follows;
  - then it waits for the next message.
- Returns `OS_TASKQ` (13) for a non-callback message; 22 on timeout (cannot happen with -1).

`os_taskq_post` (varargs) and `os_taskq_post_msg` build the same layout.

## Also seen in `os_api.c`

`sdk_mkey_lock`, `sdk_mkey_lock_v2_cfun`, `sdk_chip_key_verify_v2` and `isr_check_key` exist: SDK licence-key checks
on the chip key. If the BT libraries reference any of them, the undefined-symbol list shows it. The FM-1's chip key
is 0x980F, a real JieLi key.

## Link facts (Task 7, first `FELUCCA_BLE=1` link, step 1)

- **How the libraries are linked:** they are bitcode and go through `ld --plugin LLVMgold.so` (`lto-wrapper` is
  only a Python wrapper for that) with `mcpu=r3` and `-pi32v2-large-program=true`. They sit inside
  `--start-group … --end-group`.
- **What else is linked:**
  - Whole archives: `btctrler`, `btstack`, `wl_rf_common`, `crypto_toolbox_Osize`, `lib_ccm_aes`, and newlib's
    `libcompiler_rt` (for soft float).
  - Single members only: `system.a` `lbuf.c.o`, `circular_buf.c.o` and `cpu.a` `wlc.c.o`, `encryption.c.o`.
    The whole of `system.a` would bring the SDK's own OS, and the whole of `cpu.a` its start-up code (multiple
    definitions).
- **Constants:** the SDK's `lib_btctrler_config.c` and `bt_profile_config.c`, compiled against
  `firmware/src/ble/sdkcfg/app_config.h` (BLE central only).
- **`app_ble.ld`:** `--orphan-handling=error`. The libraries' `.*_code` / `.*_const` go in XIP, `.*_data` in .data,
  `.*_bss` / `.bd_base` / `.*_bulk` in .bss. The `lp_target` / `sys_event` tables and `.LOG_TAG_CONST` are placed
  explicitly.
- **The radio calibration runs from RAM** (fixed after device step D crashed on PLAY, mostly). `wl_rf_common`'s
  `wf_rf_trim` (`.volatile_ram_code`) and the trim routines (`*.rf.text.cache.L2.rf_trim`, 3.6 KB) are linked into
  `.ram_text`, as every SDK demo builds them (`CONFIG_RF_TRIM_CODE_AT_RAM`). The SDK's reason (`app_config.h`,
  translated): reading the flash during the ~500 ms high-current trim can make it hang. On the FM-1 it gave
  garbage instruction fetches (a jump to `0x470`, PC guard) in about half the starts.
  - `ble_port.c` gives the trim:
    - RAM `delay_us` / `hw_udelay` / `delay`;
    - `__wrap_memcpy` in RAM (`--wrap=memcpy`: only the libraries' and the BLE unit's references; the core keeps
      its own);
    - `enter` / `exit_wifi_rf_trim_region` in RAM, which turn interrupts off for the trim, since every interrupt
      handler runs from XIP.
  - The BLE unit is compiled with long calls (`-pi32v2-large-program=true`) to reach RAM.
  - `build.py` checks every build: `.ram_text` calls and jumps only inside `.ram_text`, never through a register,
    with no XIP address as data.
  - `.bt_updata_ram_code` (`bredr_frame`'s BT-over-the-air-update helpers, never run) stays in XIP, allowed by
    name; any other `*ram_code*` section fails the build.
- **IRQ handlers passed to `request_irq`.** All end in `rti`, i.e. they are interrupt functions, so `request_irq`
  puts them straight into the vector table:

  | Handler | IRQ | Priority |
  | --- | --- | --- |
  | `ble_event_irq_handler` | 45 | 2 |
  | `ble_rx_irq_handler` | 29 | 2 |
  | `slot_timer_irq_handler` | 41 | 2 |
  | `bredr_rx_irq_handler` | 40 (classic, not reached in BLE mode) | — |

- **Step-1 image: 486,440 B** (slot 581,564 B), RAM .data+.bss 28.8 KB, pool 127 KB. Of that:
  - about 103 KB is classic BR/EDR and TWS code that is never run;
  - about 77 KB is the libraries' log strings (their printf stubs print nothing).

  Trimming both is deferred.

## The update-path gates in BLE builds (why gate 2 has two parts)

The BLE module is its own compilation unit (`firmware/src/ble/ble.c`), reaching the core only through
`ble_core.h` / `core_ble_api.c`. Even so, the core's machine code cannot stay byte-identical to the stripped core.
In the stripped core nothing ever writes the USB-MIDI-out ring, so the compiler deletes `usb_poll`'s MIDI-out path
as dead code. In a BLE build the forwarding brings it back, as in every Felucca release, where the keys send MIDI.
The new wrappers also shift clang's global-merge layout and inlining in the core's unit.

**Gate 2a (`check_update_path.py --sources v1.0.5.2`)** checks that the update path's sources are Felucca 1.0.5.2's:
- the loader;
- `usb.c`, `ota.c`, `ota_hw.c`, `libc.c`, `storage*.c`;
- the shared HAL headers.

**Gate 2b** checks that the BLE build's core unit equals the reference core. The reference (`FELUCCA_CORE_REF=1`)
is the same core unit (wrappers and main-loop calls, `-enable-global-merge=false`) linked with
`ble/ble_stub.c` instead of the BLE unit and the libraries. Gate 2b compares instructions:
- calls by callee;
- branches inside the function by offset;
- data addresses by role;
- every constant and memory offset exactly.

So it proves that the BLE libraries and the module change nothing in the core's code.

What remains for the device:
- **The default (non-BLE) build** stays byte-identical to the stripped core installed as probe 1.
- **The BLE builds' update entry** (`usb_poll` with MIDI out alive) is proven on the device by the A→B install chain.
- **The loader** is pinned by hash.
