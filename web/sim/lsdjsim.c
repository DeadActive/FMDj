/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj build in the browser (web/sim/, after Hortator's simulator: its tests/sim_core.c and tests/sim_host.h,
 * and the same sim_* interface, so its engine and worklet drive this one too). The firmware's LSDj unit
 * (firmware/src/lsdj/lsdj.c) and the core's own text drawing (gfx.c, the generated fonts) are compiled as they are;
 * what stands in for the hardware and the rest of the core is here:
 *   the screen     a 240 x 240 frame buffer (lcd_fill / lcd_blit)
 *   the panel      buttons, the 27 keys, the three knobs LSDj uses, the MASTER pot: set by sim_btn / _key / _enc / _master
 *   the sound      lsdj_audio pulled in blocks of 128 frames, as the audio interrupt does
 *   time           counted in sound: sim_render is the clock; the main loop's pass every 15 ms of it
 *   the flash      1 MiB in memory with a map of written sectors (the page keeps them in the browser)
 *   the console    `ble ...` lines, as over USB (sim_con): the song page's code moves songs and saves with them
 *   the cartridge  the user's own ROM as a cartridge block (lsdj_cart.c) at the end of sim_cart(): the page puts it
 *                  there before sim_init. Nothing of LSDj is in this file or in the wasm.
 * One instance = one power-on. Built by web/sim/build.sh (Emscripten) to build/web/sim/lsdjsim.wasm. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define LSDJ_HOST 1
#define CORE_HOST_LIBC 1
#define LSDJ_POOL
#include <time.h>

#define FS 44100u
#define SIM_BLOCK 128u                         /* felucca.c HALF_FRAMES: the audio interrupt's block */
#define SIM_MAX_FRAMES 1024u
#define SIM_PASS_SAMPLES (FS * 15u / 1000u)    /* core_main.c: a pass of the main loop lasts 15 ms */
#define SIM_FLASH 0x100000u
#define SIM_SECTOR 4096u
#define SIM_CART_MAX (64u * 0x4000u + 128u * 1024u + 4096u)

/* ---- the screen (lcd.c's part): pixels arrive byte-swapped, as the panel takes them */
static uint16_t sim_lcd[240u * 240u];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            sim_lcd[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++) {
            uint16_t v = px[j * w + i];
            sim_lcd[(y + j) * 240u + x + i] = (uint16_t)(v >> 8 | v << 8);
        }
}
#include "gfx.c"                               /* the core's text and palette, as built */

/* ---- the core as the LSDj unit sees it (firmware/src/ble/ble_core.h; core_ble_api.c on the device) */
#include "ble/ble_core.h"
static uint64_t sim_samples;
static uint32_t sim_buttons, sim_prev, sim_notes, sim_master_raw = 800;
static uint32_t sim_tap_b, sim_tap_n;          /* down since the last pass that looked: a press shorter than a pass counts */
static int32_t sim_enc_acc[3];                 /* EN_SELECT, EN_ALGO, EN_PRESET (panel.c) */
static uint8_t sim_flash_b[SIM_FLASH], sim_cart_b[SIM_CART_MAX];
static uint32_t sim_dirty[SIM_FLASH / SIM_SECTOR / 32u];

uint32_t core_ms(void) { return (uint32_t)(sim_samples * 1000u / FS); }
uint32_t core_ticks(void) { return (uint32_t)(sim_samples * (CORE_TICKS_PER_US * 1000000ull) / FS); }
uint32_t core_buttons(void) { return sim_buttons | sim_tap_b; }
uint32_t core_notes(void) { return sim_notes | sim_tap_n; }
uint32_t core_button_bit(int label) { return label >= 0 && label < CORE_NBTN ? 1u << label : 0; }
uint32_t core_play_bit(void) { return 1u << CORE_BTN_PLAY; }
uint32_t core_rec_bit(void) { return 1u << CORE_BTN_REC; }
int32_t core_master_pot(void) { return (int32_t)sim_master_raw; }
static int32_t sim_take(uint32_t k)
{
    int32_t v = sim_enc_acc[k];
    sim_enc_acc[k] = 0;
    return v;
}
int32_t core_enc_steps(int knob) { return sim_take(knob == CORE_ENC_PRESET ? 2u : 1u); }
int core_lcd_idle(void) { return 1; }
/* the console (console.c on the device, over USB): the page writes a line into sim_con_in() and calls sim_con(),
 * which carries out its `ble ...` as the console does and gives what the unit printed */
static char sim_con_line[64], sim_con_out[2048];
static uint32_t sim_con_n;
void core_con_puts(const char *s)
{
    while (*s && sim_con_n + 1u < sizeof sim_con_out)
        sim_con_out[sim_con_n++] = *s++;
    sim_con_out[sim_con_n] = 0;
}
const char *core_con_line(void) { return sim_con_line; }

static uint16_t core_colour(int c)             /* (core_ble_api.c) */
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
void core_fill(int x, int y, int w, int h, int colour) { lcd_fill((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, core_colour(colour)); }
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

/* the unit's store: the same area as on the device (0xA0000 .. 0xDBFFF), a NOR flash: bits only go to 0 */
#define STORE_LO 0xA0000u
#define STORE_HI 0xDC000u
const uint8_t *core_flash_ptr(uint32_t off) { return off >= STORE_LO && off < STORE_HI ? sim_flash_b + off : 0; }
int core_flash_erase(uint32_t off)
{
    if (off < STORE_LO || off > STORE_HI - SIM_SECTOR || (off & (SIM_SECTOR - 1u)))
        return -8;
    memset(sim_flash_b + off, 0xFF, SIM_SECTOR);
    sim_dirty[off / SIM_SECTOR / 32u] |= 1u << (off / SIM_SECTOR % 32u);
    return 0;
}
int core_flash_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off < STORE_LO || off > STORE_HI || n > STORE_HI - off)
        return -8;
    for (i = 0; i < n; i++) {
        sim_flash_b[off + i] &= ((const uint8_t *)src)[i];
        sim_dirty[(off + i) / SIM_SECTOR / 32u] |= 1u << ((off + i) / SIM_SECTOR % 32u);
    }
    return 0;
}
const uint8_t *core_cart_end(void) { return sim_cart_b + SIM_CART_MAX; }
uint32_t core_cart_room(void) { return SIM_CART_MAX; }

#include "lsdj/lsdj.c"                         /* the LSDj unit, as on the device */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define SIM EMSCRIPTEN_KEEPALIVE
#else
#define SIM
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "simulator"
#endif

/* ---- the interface the page drives (Hortator's tests/sim_core.c) */
static float sim_out[2u * SIM_MAX_FRAMES];
static int32_t sim_blk[2u * SIM_BLOCK];
static uint32_t sim_pos = SIM_BLOCK;
static uint64_t sim_next_pass;

static void sim_pass(void)                     /* core_main.c's loop body, minus the hardware */
{
    uint32_t now = sim_buttons | sim_tap_b, down = now & ~sim_prev, f = st.frames;
    sim_prev = sim_buttons;
    ble_key(down, sim_take(0));
    ble_service();
    ble_draw();
    if (!ble_started() || st.frames != f)      /* (a Game Boy frame has read the keys, or none is to come) */
        sim_tap_b = sim_tap_n = 0;
}

SIM void sim_flash_reset(void)
{
    memset(sim_flash_b, 0xFF, sizeof sim_flash_b);
    memset(sim_dirty, 0, sizeof sim_dirty);
}

SIM void sim_init(int fresh)                   /* core_main.c's boot screen; the page has put the flash and the cartridge */
{
    if (fresh)
        sim_flash_reset();
    palette_set(UI_GREY_INDEX);
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 94, 240, &AF_L, "LSDJ", T_THEME, 1);
    draw_text_box(0, 134, 240, &AF_S, FELUCCA_VERSION, T_MID, 1);
    sim_next_pass = sim_samples + SIM_PASS_SAMPLES;
}

SIM uint32_t sim_render(uint32_t frames)       /* interleaved float stereo into sim_audio(); the passes run */
{
    uint32_t i, passes = 0;
    if (frames > SIM_MAX_FRAMES)
        frames = SIM_MAX_FRAMES;
    for (i = 0; i < frames; i++) {
        if (sim_pos == SIM_BLOCK) {
            if (sim_samples >= sim_next_pass) {            /* between two blocks, as the main loop runs */
                sim_pass();
                passes++;
                sim_next_pass += SIM_PASS_SAMPLES;
            }
            lsdj_audio(sim_blk, SIM_BLOCK);                /* Q15 */
            sim_samples += SIM_BLOCK;
            sim_pos = 0;
        }
        sim_out[2u * i] = (float)sim_blk[2u * sim_pos] * (1.0f / 32768.0f);
        sim_out[2u * i + 1u] = (float)sim_blk[2u * sim_pos + 1u] * (1.0f / 32768.0f);
        sim_pos++;
    }
    return passes;
}

SIM void sim_btn(uint32_t label, int down)     /* label: panel.c B_FX .. B_OCTUP = ble_core.h CORE_BTN_* */
{
    if (label >= (uint32_t)CORE_NBTN)
        return;
    if (down) {
        sim_buttons |= 1u << label;
        sim_tap_b |= 1u << label;
    } else
        sim_buttons &= ~(1u << label);
}
SIM void sim_key(uint32_t n, int down)         /* note key 0 (F3) .. 26 (G5) */
{
    if (n >= 27u)
        return;
    if (down) {
        sim_notes |= 1u << n;
        sim_tap_n |= 1u << n;
    } else
        sim_notes &= ~(1u << n);
}
SIM void sim_enc(uint32_t role, int32_t steps) /* role: panel.c EN_SELECT, EN_ALGO, EN_PRESET (the others: nothing); + = clockwise */
{
    if (role < 3u)
        sim_enc_acc[role] += steps;
}
SIM void sim_master(uint32_t v) { sim_master_raw = v > 1023u ? 1023u : v; }
SIM uint32_t sim_leds(void) { return 0; }      /* (the LSDj unit lights none) */
SIM uint32_t sim_key_leds(void) { return 0; }
SIM uint32_t sim_playing(void) { return (uint32_t)ble_started(); }
SIM uint16_t *sim_fb(void) { return sim_lcd; }
SIM float *sim_audio(void) { return sim_out; }
SIM uint8_t *sim_flash(void) { return sim_flash_b; }
SIM uint32_t *sim_flash_dirty(void) { return sim_dirty; }
SIM void sim_flash_clean(void) { memset(sim_dirty, 0, sizeof sim_dirty); }
SIM uint8_t *sim_cart(void) { return sim_cart_b; }         /* the block goes at its end: sim_cart() + sim_cart_max() - size */
SIM uint32_t sim_cart_max(void) { return SIM_CART_MAX; }
SIM void sim_cart_clear(void) { memset(sim_cart_b, 0xFF, sizeof sim_cart_b); }
SIM char *sim_con_in(void) { return sim_con_line; }        /* the line: up to 63 characters and a 0 */
SIM const char *sim_con(void)                              /* -> its output (the console's own prompt is the page's) */
{
    sim_con_line[sizeof sim_con_line - 1u] = 0;
    sim_con_n = 0;
    sim_con_out[0] = 0;
    ble_status();
    return sim_con_out;
}
SIM uint32_t sim_frames(void) { return st.frames; }        /* Game Boy frames run (tests) */
SIM int32_t sim_error(void) { return st.error; }           /* the unit's: -3 no ROM, -4 a damaged one */
