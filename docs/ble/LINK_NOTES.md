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

## Still to record (Task 7, at the first `FELUCCA_BLE=1` link)

- `undefined-step0.txt`: the symbols the BT libraries need from outside, against the core.
- The `request_irq` handlers for IRQ 29 / 45: whether they are interrupt functions (ending in `rti`) or plain C
  functions (they then need the `fm1_isr.S` wrapper).
