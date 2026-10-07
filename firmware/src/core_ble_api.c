/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* FELUCCA_BLE: the core functions the BLE unit calls (firmware/src/ble/ble_core.h). Thin wrappers over the core's
 * static functions and data; no static data of their own, so the core's other functions compile as without BLE. */
#include "ble/ble_core.h"
uint32_t core_ms(void) { return fm1_ms; }
void core_wdt_feed(void) { fm1_wdt_feed(); }
void core_reboot(void) { fm1_reboot(); }
void core_audio_stop(void) { fm1_audio_stop(); }
void core_irq_attach(uint32_t n, void (*h)(void), uint32_t prio) { fm1_irq_attach(n, h, prio); }
void core_irq_mask(uint32_t n) { fm1_irq_mask(n); }
void core_irq_unmask(uint32_t n) { fm1_irq_unmask(n); }
uint32_t core_irq_is_masked(uint32_t n) { return fm1_irq_is_masked(n); }
void core_midi_out(uint32_t pkt) { midi_out_event(pkt); }
static uint16_t core_colour(int c)
{
    switch (c) {
    case CORE_TEXT: return T_TEXT;
    case CORE_THEME: return T_THEME;
    case CORE_MID: return T_MID;
    case CORE_SEL: return T_SEL;
    case CORE_INK: return T_INK;
    case CORE_CRASH_BG: return UI_CRASH_BG;
    case CORE_CRASH_INK: return UI_CRASH_INK;
    default: return T_BG;
    }
}
void core_fill(int x, int y, int w, int h, int colour) { lcd_fill(x, y, w, h, core_colour(colour)); }
void core_text(int x, int y, int w, int font, const char *s, int fg, int bg, int centre)
{
    draw_text_line((uint32_t)x, (uint32_t)y, (uint32_t)w, font == CORE_FONT_M ? &AF_M : &AF_S, s, core_colour(fg),
                   core_colour(bg), centre);
}
uint32_t core_buttons(void) { return fm1_in.buttons; }
uint32_t core_play_bit(void) { return 1u << panel.btn[B_PLAY]; }
uint32_t core_rec_bit(void) { return 1u << panel.btn[B_REC]; }
int32_t core_select_steps(void) { return panel_enc(EN_SELECT); }
void core_con_puts(const char *s)
{
#if FELUCCA_CDC
    con_puts(s);
#else
    (void)s;
#endif
}
