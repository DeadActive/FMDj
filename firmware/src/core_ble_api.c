/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* FELUCCA_BLE: the core functions the BLE unit calls (firmware/src/ble/ble_core.h). Thin wrappers over the core's
 * static functions and data; no static data of their own, so the core's other functions compile as without BLE. */
#include "ble/ble_core.h"
extern uint32_t _pool_start[];
uint32_t core_ms(void) { return fm1_ms; }
void core_wdt_feed(void) { fm1_wdt_feed(); }
void core_reboot(void) { fm1_reboot(); }
void core_audio_stop(void) { fm1_audio_stop(); }
void core_irq_attach(uint32_t n, void (*h)(void), uint32_t prio)   /* IRQs off (the caller) */
{
    fm1_guard_unlock_top();                     /* the vector table is write-protected after boot */
    fm1_irq_attach(n, h, prio);
    fm1_guard_lock_top();
}
void core_ble_stack_window(void)                /* the BLE tasks' stacks are in .pool, below the main stacks */
{
    fm1_guard_stack_window((uint32_t)(uintptr_t)_pool_start, (uint32_t)(uintptr_t)_sstack_top);
}
void core_irq_mask(uint32_t n) { fm1_irq_mask(n); }
void core_irq_unmask(uint32_t n) { fm1_irq_unmask(n); }
uint32_t core_irq_is_masked(uint32_t n) { return fm1_irq_is_masked(n); }
void core_midi_out(uint32_t pkt) { midi_out_event(pkt); }
int core_flash_read(uint32_t off, void *buf, uint32_t n)    /* plain flash only (0x93000..), read only */
{
#if FELUCCA_FLASH
    if (!flash_ok || off < 0x93000u || off > 0x100000u || n > 0x100000u - off)
        return -1;
    return st_read(off, buf, n);
#else
    (void)off;
    (void)buf;
    (void)n;
    return -1;
#endif
}
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
void core_lcd_blit(int x, int y, int w, int h, const uint16_t *px)
{
    if (x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= 240 && y + h <= 240)
        lcd_blit((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, px);
}
uint32_t core_ticks(void) { return fm1_ticks(); }
int core_lcd_idle(void) { return !lcd_busy || fm1_lcd_done(); }
int32_t core_master_pot(void) { return fm1_adc_read(FM1_ADC_MASTER); }
uint32_t core_notes(void) { return fm1_in.notes; }
uint32_t core_buttons(void) { return fm1_in.buttons; }
_Static_assert(CORE_BTN_FX == B_FX && CORE_BTN_SCL == B_SCL && CORE_BTN_EDIT == B_EDIT && CORE_BTN_GLO == B_GLO &&
               CORE_BTN_HOME == B_HOME && CORE_BTN_SAVE == B_SAVE && CORE_BTN_ARP == B_ARP && CORE_BTN_PLAY == B_PLAY &&
               CORE_BTN_REC == B_REC && CORE_BTN_OCTUP == B_OCTUP && CORE_NBTN == NB, "ble_core.h CORE_BTN_* = B_*");
uint32_t core_button_bit(int label) { return label >= 0 && label < NB ? 1u << panel.btn[label] : 0; }
uint32_t core_play_bit(void) { return 1u << panel.btn[B_PLAY]; }
uint32_t core_rec_bit(void) { return 1u << panel.btn[B_REC]; }
int32_t core_select_steps(void) { return panel_enc(EN_SELECT); }
int32_t core_enc_steps(int knob) { return panel_enc(knob == CORE_ENC_PRESET ? EN_PRESET : EN_ALGO); }
void core_con_puts(const char *s)
{
#if FELUCCA_CDC
    con_puts(s);
#else
    (void)s;
#endif
}
const char *core_con_line(void)
{
#if FELUCCA_CDC
    return con.line;
#else
    return "";
#endif
}
