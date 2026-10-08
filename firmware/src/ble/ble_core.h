/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* The core functions the BLE unit (firmware/src/ble/ble.c, its own object) may call: defined in
 * firmware/src/core_ble_api.c inside the core's unit. The BLE unit shares no static data with the core, so the core
 * compiles as it does without BLE (gate 2: tools/check_update_path.py). And the BLE entry points the core calls. */
#ifndef BLE_CORE_H
#define BLE_CORE_H
#include <stdint.h>
/* time, watchdog, power */
uint32_t core_ms(void);
void core_wdt_feed(void);
void core_reboot(void);
void core_audio_stop(void);
/* interrupts (fm1_irq.h) */
void core_irq_attach(uint32_t n, void (*h)(void), uint32_t prio);
void core_irq_mask(uint32_t n);
void core_irq_unmask(uint32_t n);
uint32_t core_irq_is_masked(uint32_t n);
/* guards (fm1_guard.h): widen the stack-limit window to the BLE task stacks (before the first task runs) */
void core_ble_stack_window(void);
/* USB MIDI out (usb.c midi_out_event: one USB-MIDI event packet, cable 0) */
void core_midi_out(uint32_t pkt);
/* screen: colours and fonts by name */
enum { CORE_BG, CORE_TEXT, CORE_THEME, CORE_MID, CORE_SEL, CORE_INK, CORE_CRASH_BG, CORE_CRASH_INK };
enum { CORE_FONT_S, CORE_FONT_M };
void core_fill(int x, int y, int w, int h, int colour);
void core_text(int x, int y, int w, int font, const char *s, int fg, int bg, int centre);
/* keys */
uint32_t core_buttons(void);
uint32_t core_play_bit(void);
uint32_t core_rec_bit(void);
int32_t core_select_steps(void);
/* console (CDC) */
void core_con_puts(const char *s);
/* libc.c */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);

/* the BLE unit's entry points (ble.c) */
void ble_service(void);
void ble_key(uint32_t pressed_buttons, int32_t select_steps);
void ble_draw(void);
void ble_status(void);
int ble_started(void);
#endif
