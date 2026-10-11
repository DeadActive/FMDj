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
/* flash: read the plain area (0x93000..0xFFFFF) through the core's driver; 0 = ok (never writes) */
int core_flash_read(uint32_t off, void *buf, uint32_t n);
/* the LSDj unit's store: plain flash inside Felucca's user-sample area only (0xA0000..0xDBFFF). ptr: the area as
 * memory (0 outside it, or without flash). erase: one 4 KiB sector; prog: any length, from RAM. Both 0 = done; they
 * run with interrupts off through the driver's RAM routines, as the core's own storage does. */
const uint8_t *core_flash_ptr(uint32_t off);
int core_flash_erase(uint32_t off);
int core_flash_prog(uint32_t off, const void *src, uint32_t n);
/* screen: colours and fonts by name */
enum { CORE_BG, CORE_TEXT, CORE_THEME, CORE_MID, CORE_SEL, CORE_INK, CORE_CRASH_BG, CORE_CRASH_INK };
enum { CORE_FONT_S, CORE_FONT_M };
void core_fill(int x, int y, int w, int h, int colour);
void core_text(int x, int y, int w, int font, const char *s, int fg, int bg, int centre);
void core_lcd_blit(int x, int y, int w, int h, const uint16_t *px);   /* RGB565 byte-swapped, px in RAM (DMA,
                                                                     * still running on return: keep px until the
                                                                     * next LCD call) */
int core_lcd_idle(void);                      /* 1: no transfer running, the next LCD call will not wait */
/* the MASTER pot: its 10-bit reading (0..1023), -1 on an ADC timeout (hal/fm1_adc.h) */
int32_t core_master_pot(void);
/* time: TIMER4 ticks */
#define CORE_TICKS_PER_US 24u
uint32_t core_ticks(void);
/* keys */
uint32_t core_notes(void);                    /* note keys held: bit 0 = F3 .. 26 = G5 */
uint32_t core_buttons(void);
/* the panel's button labels (panel.c's B_*, same order: checked in core_ble_api.c) and the bit of each in
 * core_buttons() */
enum { CORE_BTN_FX, CORE_BTN_SCL, CORE_BTN_ENV, CORE_BTN_LFO, CORE_BTN_EDIT, CORE_BTN_GLO, CORE_BTN_HOME, CORE_BTN_SAVE,
       CORE_BTN_ARP, CORE_BTN_SEQ, CORE_BTN_PLAY, CORE_BTN_REC, CORE_BTN_OCTDN, CORE_BTN_OCTUP, CORE_NBTN };
uint32_t core_button_bit(int label);
uint32_t core_play_bit(void);
uint32_t core_rec_bit(void);
int32_t core_select_steps(void);
/* the ALGORITHM and PRESETS knobs: steps since the last call, + = clockwise (the SELECT knob's come with ble_key) */
enum { CORE_ENC_ALGO, CORE_ENC_PRESET };
int32_t core_enc_steps(int knob);
/* the core's crash record (fm1_irq.h fm1_crash_t, .noinit, same layout): read only, for the console's `ble`
 * (the core's unit has the real declaration) */
#ifndef FM1_CRASH_MAGIC
extern const struct {
    uint32_t magic, count, vec, pc, rets, emu, dbg, sp, psr, icfg, uptime_ms;
    uint32_t etm[4];
    uint32_t early;
} fm1_crash;
#endif
/* console (CDC) */
void core_con_puts(const char *s);
/* the console line being carried out ("ble ...": the words after `ble` are the unit's); "" without a console */
const char *core_con_line(void);
/* libc.c (host tests: <string.h>) */
#ifndef CORE_HOST_LIBC
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
#endif

/* the BLE unit's entry points (ble.c) */
void ble_service(void);
void ble_key(uint32_t pressed_buttons, int32_t select_steps);
void ble_draw(void);
void ble_status(void);
int ble_started(void);
/* the LSDj unit (FELUCCA_LSDJ, firmware/src/lsdj/lsdj.c) in that place: also the audio ISR's sound source */
void lsdj_audio(int32_t *out, uint32_t n);
#endif
