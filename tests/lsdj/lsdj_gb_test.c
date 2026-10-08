/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* Host tests of firmware/src/lsdj/lsdj_gb.c, the LSDj unit's pure parts: the ROM bank table, the key map, the
 * frame buffer and its dirty spans, the 1.5x scaler, the audio ring, the save unpacker. No LSDj data here.
 *   cc -O1 -Wall -Ifirmware/src -o build/host/lsdj_gb_test tests/lsdj/lsdj_gb_test.c && build/host/lsdj_gb_test */
#include <stdio.h>
#include <string.h>
#include "lsdj/lsdj_gb.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_rom(void)
{
    static const uint8_t b0[3] = {0x11, 0x22, 0x33}, b2[1] = {0x44};
    const lsdj_bank_t t[3] = {{b0, 3}, {0, 0}, {b2, 1}};
    CHECK(lsdj_rom_byte(t, 3, 0) == 0x11);
    CHECK(lsdj_rom_byte(t, 3, 2) == 0x33);
    CHECK(lsdj_rom_byte(t, 3, 3) == 0xFF);              /* past the stored bytes: the trimmed 0xFF tail */
    CHECK(lsdj_rom_byte(t, 3, 0x3FFF) == 0xFF);
    CHECK(lsdj_rom_byte(t, 3, 0x4000) == 0xFF);         /* an empty (all 0xFF) bank */
    CHECK(lsdj_rom_byte(t, 3, 0x8000) == 0x44);
    CHECK(lsdj_rom_byte(t, 3, 0x8001) == 0xFF);
    CHECK(lsdj_rom_byte(t, 3, 0xC000) == 0xFF);         /* past the last bank */
    CHECK(lsdj_rom_byte(t, 3, 0xFFFFFFu) == 0xFF);
}

static void test_pad(void)
{
    const uint32_t PLAY = 1u << 10, REC = 1u << 11;
    CHECK(lsdj_pad(0, 0, PLAY, REC) == 0xFF);                       /* active low: nothing held */
    CHECK(lsdj_pad(PLAY, 0, PLAY, REC) == (uint8_t)~JOYPAD_START);
    CHECK(lsdj_pad(REC, 0, PLAY, REC) == (uint8_t)~JOYPAD_SELECT);
    CHECK(lsdj_pad(0, 1u << 0, PLAY, REC) == (uint8_t)~JOYPAD_LEFT);    /* F3 */
    CHECK(lsdj_pad(0, 1u << 2, PLAY, REC) == (uint8_t)~JOYPAD_DOWN);    /* G3 */
    CHECK(lsdj_pad(0, 1u << 3, PLAY, REC) == (uint8_t)~JOYPAD_UP);      /* G#3 */
    CHECK(lsdj_pad(0, 1u << 4, PLAY, REC) == (uint8_t)~JOYPAD_RIGHT);   /* A3 */
    CHECK(lsdj_pad(0, 1u << 24, PLAY, REC) == (uint8_t)~JOYPAD_B);      /* F5 */
    CHECK(lsdj_pad(0, 1u << 26, PLAY, REC) == (uint8_t)~JOYPAD_A);      /* G5 */
    CHECK(lsdj_pad(0, (1u << 1) | (1u << 25), PLAY, REC) == 0xFF);      /* other keys do nothing */
    CHECK(lsdj_pad(PLAY | REC, (1u << 24) | (1u << 26), PLAY, REC) ==
          (uint8_t)~(JOYPAD_START | JOYPAD_SELECT | JOYPAD_A | JOYPAD_B));   /* combinations */
    CHECK(lsdj_pad(1u << 0 | 1u << 1, 0, PLAY, REC) == 0xFF);           /* OCT- / OCT+ are not Game Boy keys */
}

static void test_pad_text(void)
{
    char b[32];
    lsdj_pad_text(b, 0xFF);
    CHECK(strcmp(b, "-") == 0);                               /* nothing held */
    lsdj_pad_text(b, (uint8_t)~JOYPAD_START);
    CHECK(strcmp(b, "START") == 0);
    lsdj_pad_text(b, (uint8_t)~(JOYPAD_SELECT | JOYPAD_RIGHT));
    CHECK(strcmp(b, "SEL R") == 0);
    lsdj_pad_text(b, 0x00);                                   /* all: the longest text */
    CHECK(strcmp(b, "START SEL A B L R U D") == 0);
}

static uint8_t fb[LSDJ_H][LSDJ_W], shown[LSDJ_H][LSDJ_W];

static void test_line_and_span(void)
{
    uint8_t px[LSDJ_W];
    uint32_t x0, x1, i;
    memset(fb, 0, sizeof fb);
    memset(shown, 0, sizeof shown);
    for (i = 0; i < LSDJ_W; i++)
        px[i] = (uint8_t)(0x20u | (i & 3u));                  /* palette flag bits are dropped */
    lsdj_line(fb[5], px);
    CHECK(fb[5][0] == 0 && fb[5][1] == 1 && fb[5][3] == 3 && fb[5][159] == 3);
    CHECK(!lsdj_span(fb[0], shown[0], &x0, &x1));             /* group 0 (lines 0, 1): clean */
    CHECK(lsdj_span(fb[4], shown[4], &x0, &x1));              /* group 2 (lines 4, 5): line 5 changed */
    CHECK(x0 == 0 && x1 == 160);
    memcpy(shown[5], fb[5], LSDJ_W);
    fb[4][7] = 2;                                             /* one pixel: rounded out to a pixel pair */
    CHECK(lsdj_span(fb[4], shown[4], &x0, &x1));
    CHECK(x0 == 6 && x1 == 8);
    fb[5][100] = 1;
    CHECK(lsdj_span(fb[4], shown[4], &x0, &x1));
    CHECK(x0 == 6 && x1 == 102);
    shown[4][7] = 2;
    shown[5][100] = 1;
    CHECK(!lsdj_span(fb[4], shown[4], &x0, &x1));
}

static void test_scale(void)
{
    static const uint16_t pal[4] = {0xA000, 0xB000, 0xC000, 0xD000};
    uint16_t out[3 * 240];
    uint8_t two[2][LSDJ_W];
    uint32_t i;
    for (i = 0; i < LSDJ_W; i++) {
        two[0][i] = (uint8_t)(i & 3u);
        two[1][i] = (uint8_t)(3u - (i & 3u));
    }
    lsdj_scale(out, two[0], 0, 4, pal);                       /* 4 pixels -> 6 wide, 3 rows */
    /* row 0 and 1 from line 0: a a b  c c d */
    CHECK(out[0] == pal[0] && out[1] == pal[0] && out[2] == pal[1] && out[3] == pal[2] && out[4] == pal[2] &&
          out[5] == pal[3]);
    CHECK(memcmp(out + 6, out, 6 * sizeof out[0]) == 0);
    /* row 2 from line 1 */
    CHECK(out[12] == pal[3] && out[13] == pal[3] && out[14] == pal[2] && out[17] == pal[0]);
    lsdj_scale(out, two[0], 2, 6, pal);                       /* a span not at 0: starts at its own pixel */
    CHECK(out[0] == pal[2] && out[2] == pal[3] && out[3] == pal[0] && out[5] == pal[1]);
    CHECK(LSDJ_SX(2) == 3 && LSDJ_SX(160) == 240);
}

static void test_ring(void)
{
    static int16_t buf[16 * 2];
    static lsdj_ring_t r;
    int16_t in[2 * 10];
    int32_t out[2 * 16];
    uint32_t i;
    lsdj_ring_init(&r, buf, 16);
    CHECK(lsdj_ring_free(&r) == 15);                          /* one slot kept empty */
    for (i = 0; i < 20; i++)
        in[i] = (int16_t)(i * 100 - 1000);
    lsdj_ring_push(&r, in, 10);
    CHECK(lsdj_ring_free(&r) == 5);
    CHECK(lsdj_ring_fill(&r) == 10);
    lsdj_ring_pull(&r, out, 4);
    CHECK(out[0] == -1000 && out[1] == -900 && out[6] == -400 && out[7] == -300);
    CHECK(r.under == 0);
    lsdj_ring_push(&r, in, 9);                                /* wraps */
    CHECK(lsdj_ring_fill(&r) == 15);
    lsdj_ring_pull(&r, out, 15);
    CHECK(out[0] == -200 && out[2 * 5] == 800 && out[2 * 6] == -1000 && out[2 * 14 + 1] == (int16_t)(17 * 100 - 1000));
    lsdj_ring_pull(&r, out, 3);                               /* empty: holds the last sample, counts */
    CHECK(r.under == 3);
    CHECK(out[0] == 600 && out[1] == 700 && out[4] == 600 && out[5] == 700);
}

static void test_unpack(void)
{
    /* PackBits: n < 128 -> n + 1 literal bytes; n >= 128 -> (n & 127) + 1 copies of the next byte */
    static const uint8_t src[] = {0x82, 0x00, 0x01, 0xAA, 0xBB, 0xFF, 0x07};
    uint8_t dst[3 + 2 + 128 + 4];
    memset(dst, 0x55, sizeof dst);
    CHECK(lsdj_unpack(dst, 3 + 2 + 128, src, sizeof src) == 0);
    CHECK(dst[0] == 0 && dst[2] == 0 && dst[3] == 0xAA && dst[4] == 0xBB && dst[5] == 7 && dst[132] == 7);
    CHECK(dst[133] == 0x55);                                  /* nothing written past the end */
    CHECK(lsdj_unpack(dst, 4, src, sizeof src) != 0);         /* longer than the target: refused */
    CHECK(lsdj_unpack(dst, 3 + 2 + 128 + 1, src, sizeof src) != 0);   /* short: refused */
    CHECK(lsdj_unpack(dst, 3, src, 1) != 0);                  /* a run without its byte */
}

int main(void)
{
    test_rom();
    test_pad();
    test_pad_text();
    test_line_and_span();
    test_scale();
    test_ring();
    test_unpack();
    printf(fails ? "lsdj_gb_test: %d FAILED\n" : "lsdj_gb_test: all passed\n", fails);
    return fails != 0;
}
