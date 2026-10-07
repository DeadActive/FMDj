/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* The BLE module: its own compilation unit (tools/build.py, FELUCCA_BLE=1), so the core's unit compiles exactly as
 * without BLE (gate 2). It reaches the core only through ble_core.h (firmware/src/core_ble_api.c).
 * FELUCCA_BLE_STEP: 1 linked only, 2 + start and scan, 3 + list, 4 + connect, 5 + monitor and USB forwarding. */
#include <stdint.h>
#ifndef FELUCCA_BLE_STEP
#define FELUCCA_BLE_STEP 1
#endif
#include "fm1_time.h"
#include "fm1_ble_hal.h"
#include "ble/ble_core.h"
#include "ble/ble_sdk_abi.h"
#include "ble/ble_sdk.h"
uint32_t ble_os_now_ms(void) { return core_ms(); }
#include "ble/ble_os.c"
#include "ble/ble_midi.c"
#include "ble/ble_scan.c"
#include "ble/ble_port.c"
static uint8_t ble_heap[48 * 1024] __attribute__((section(".pool")));
static uint8_t ble_on;
int ble_started(void) { return ble_on; }
#if FELUCCA_BLE_STEP < 2
/* step 1: the stack is linked (btstack_init pulls in the libraries) but nothing calls this */
static void ble_on_event(int from, struct bt_event *e) { (void)from; (void)e; }
void ble_start(void)
{
    ble_os_init(ble_heap, sizeof ble_heap);
    ble_on = 1;
    btstack_init();
}
void ble_key(uint32_t pressed_buttons, int32_t select_steps) { (void)pressed_buttons; (void)select_steps; }
void ble_draw(void) {}
void ble_status(void) { core_con_puts("ble: step 1 (linked, never started)\r\n"); }
void ble_service(void)
{
    if (ble_on)
        ble_os_service(50000);
}
#else
#include "ble/ble_central.c"
#include "ble/ble_ui.c"
void ble_service(void)
{
    if (!ble_on)
        return;
    ble_os_service(50000);                         /* at most ~50 ms per main-loop pass */
    ble_tick();                                    /* deadlines: scan end, start / connect / discovery timeouts */
    ui_midi();
}
#endif
