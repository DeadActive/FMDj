/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* Host test of the LSDj unit (firmware/src/lsdj/lsdj.c) as the core drives it: its hooks called like the main loop
 * and the audio ISR do, the core API stubbed with a 240x240 screen in RAM. Needs the private header made from the
 * owner's ROM (build/lsdj/lsdj_rom.h, tools/lsdj_rom.py); skipped without it. Writes build/lsdj/unit_screen.ppm.
 *   cc -O1 -w -Ifirmware/src -Ifirmware/src/lsdj -Ibuild/lsdj -o build/host/lsdj_unit_test tests/lsdj/lsdj_unit_test.c */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LSDJ_HOST 1
#define CORE_HOST_LIBC 1
#define LSDJ_POOL
#include <time.h>

static uint16_t screen[240][240];
static uint32_t ms, ticks, buttons, notes, blits, bad_blits, texts;
static const uint16_t *last_px;
static int last_px_busy;                      /* the stub's "DMA": px must not change until the next LCD call */
static uint16_t last_copy[3 * 240];
static uint32_t last_n;

#include "ble/ble_core.h"
uint32_t core_ms(void) { return ms; }
uint32_t core_ticks(void) { return ticks += 24u * 20000u; }    /* each call: 20 ms later */
static void dma_check(void)
{
    if (last_px_busy && memcmp(last_px, last_copy, last_n * 2u))
        bad_blits++;                          /* the source changed while its transfer could still run */
    last_px_busy = 0;
}
void core_lcd_blit(int x, int y, int w, int h, const uint16_t *px)
{
    int i, j;
    dma_check();
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 240 || y + h > 240) {
        bad_blits++;
        return;
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            screen[y + j][x + i] = px[j * w + i];
    last_px = px;
    last_n = (uint32_t)(w * h);
    memcpy(last_copy, px, last_n * 2u);
    last_px_busy = 1;
    blits++;
}
void core_fill(int x, int y, int w, int h, int colour) { (void)x; (void)y; (void)w; (void)h; (void)colour; dma_check(); }
void core_text(int x, int y, int w, int font, const char *s, int fg, int bg, int c)
{
    (void)x; (void)y; (void)w; (void)font; (void)fg; (void)bg; (void)c;
    dma_check();
    texts++;
    (void)s;
}
uint32_t core_buttons(void) { return buttons; }
uint32_t core_notes(void) { return notes; }
uint32_t core_button_bit(int label) { return 1u << label; }   /* (the stub: matrix id = label) */
uint32_t core_play_bit(void) { return 1u << 10; }
uint32_t core_rec_bit(void) { return 1u << 11; }
void core_con_puts(const char *s) { fputs(s, stdout); }

#if __has_include("lsdj_rom.h")
#include "lsdj/lsdj.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void pass(void)                        /* one main-loop pass, then 20 ms of the audio ISR */
{
    static int32_t out[2 * 32];
    uint32_t i;
    ble_service();
    ble_draw();
    for (i = 0; i < 882u; i += 32u)           /* 20 ms at 44.1 kHz in blocks of 32 (as CTL) */
        lsdj_audio(out, 32);
    ms += 20;
}

int main(void)
{
    uint32_t i, x, y, diff = 0;
    int32_t out[64];
    ble_draw();                               /* the splash, before PLAY */
    CHECK(texts == 5 && !ble_started());
    for (i = 0; i < 50; i++)
        pass();
    CHECK(!ble_started() && st.frames == 0);  /* nothing runs before PLAY */
    lsdj_audio(out, 32);
    CHECK(out[0] == 0 && out[63] == 0);
    buttons = core_play_bit();
    ble_key(buttons, 0);                      /* PLAY: start */
    buttons = 0;
    CHECK(ble_started() && st.error == 0);
    for (i = 0; i < 300; i++)                 /* 6 s */
        pass();
    CHECK(st.frames > 250 && emu.errors == 0);
    {   /* PLAY and REC held: SELECT and START reach the Game Boy, and the keys part of the status line is redrawn */
        uint32_t t = texts;
        buttons = core_play_bit() | core_rec_bit();
        pass();
        pass();
        CHECK(st.pad == (uint8_t)~(JOYPAD_START | JOYPAD_SELECT) && st.pad_shown == st.pad && texts > t);
        buttons = core_button_bit(CORE_BTN_SCL) | core_button_bit(CORE_BTN_GLO);   /* UP + A */
        pass();
        CHECK(st.pad == (uint8_t)~(JOYPAD_UP | JOYPAD_A));
        buttons = core_button_bit(CORE_BTN_OCTDN) | core_button_bit(CORE_BTN_OCTUP); /* the UBOOT pair: no key */
        pass();
        CHECK(st.pad == 0xFF);
        CHECK(emu_mode == (EMU_FAST_ROM | EMU_FRAME_SKIP));                          /* both options on at start */
        ble_key(core_button_bit(CORE_BTN_FX), 0);                                    /* FX: frame skip off */
        CHECK(emu_mode == EMU_FAST_ROM);
        ble_key(core_button_bit(CORE_BTN_LFO), 0);                                   /* LFO: fast ROM read off */
        CHECK(emu_mode == 0);
        t = texts;
        pass();
        CHECK(texts > t);                                                            /* the options redrawn */
        ble_key(core_button_bit(CORE_BTN_FX) | core_button_bit(CORE_BTN_LFO), 0);
        CHECK(emu_mode == (EMU_FAST_ROM | EMU_FRAME_SKIP));
        buttons = 0;
        pass();
        pass();
        CHECK(st.pad == 0xFF);
    }
    for (i = 0; i < 200; i++) {               /* draw passes only: the screen catches up */
        ble_draw();
    }
    CHECK(bad_blits == 0);
    for (y = 0; y < LSDJ_H; y++)              /* every Game Boy pixel on the screen, 1.5x */
        for (x = 0; x < LSDJ_W; x++)
            diff += screen[Y0 + y * 3u / 2u + (y & 1u)][x * 3u / 2u + (x & 1u)] != PAL[fb[y][x]];
    CHECK(diff == 0);
    CHECK(memcmp(fb, shown, sizeof fb) == 0);
    {
        FILE *f = fopen("build/lsdj/unit_screen.ppm", "wb");
        if (f) {
            fprintf(f, "P6 240 240 255\n");
            for (y = 0; y < 240; y++)
                for (x = 0; x < 240; x++) {
                    uint16_t c = (uint16_t)(screen[y][x] >> 8 | screen[y][x] << 8);
                    fputc((c >> 11) << 3, f);
                    fputc(((c >> 5) & 63) << 2, f);
                    fputc((c & 31) << 3, f);
                }
            fclose(f);
        }
    }
    ble_status();
    printf(fails ? "lsdj_unit_test: %d FAILED\n" : "lsdj_unit_test: all passed (%u blits)\n",
           fails ? (unsigned)fails : blits);
    return fails != 0;
}
#else
int main(void)
{
    printf("lsdj_unit_test: skipped (no build/lsdj/lsdj_rom.h: tools/lsdj_rom.py with your own ROM)\n");
    return 0;
}
#endif
