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

/* a made-up header (bytes 0x134..0x14F) of each kind: no ROM data, only the fields the programs are told apart by */
static void header(uint8_t *h, const char *title, uint8_t type, uint8_t ram, uint16_t sum)
{
    memset(h, 0, LSDJ_HDR);
    memcpy(h, title, strlen(title));
    h[0x147 - 0x134] = type;
    h[0x149 - 0x134] = ram;
    h[0x14E - 0x134] = (uint8_t)(sum >> 8);
    h[0x14F - 0x134] = (uint8_t)sum;
}

static void test_prog(void)
{
    uint8_t h[LSDJ_HDR];
    header(h, "LSDJ-ANY", 0x1B, 0x04, 0x1234);                 /* LSDj: by its cartridge, whatever its title */
    CHECK(lsdj_prog(h) == LSDJ_PROG_LSDJ && !lsdj_prog_steps(h));
    CHECK(lsdj_prog_save(LSDJ_PROG_LSDJ) == 128u * 1024u);
    header(h, "DRUMMACHINA", 0x1E, 0x03, 0xEE3C);              /* DMGo v1: its title, its cartridge, the step keys */
    CHECK(lsdj_prog(h) == LSDJ_PROG_DMGO && lsdj_prog_steps(h));
    CHECK(lsdj_prog_save(LSDJ_PROG_DMGO) == 32u * 1024u);
    header(h, "DRUMMACHINA", 0x1E, 0x03, 0xEE3D);              /* another DMGo: runs, but no step keys */
    CHECK(lsdj_prog(h) == LSDJ_PROG_DMGO && !lsdj_prog_steps(h));
    header(h, "DRUMMACHINA", 0x1B, 0x04, 0xEE3C);              /* that title on LSDj's cartridge: LSDj's rules */
    CHECK(lsdj_prog(h) == LSDJ_PROG_LSDJ && !lsdj_prog_steps(h));
    header(h, "DRUMMACHINAX", 0x1E, 0x03, 0xEE3C);             /* another title on DMGo's cartridge: neither */
    CHECK(lsdj_prog(h) == LSDJ_PROG_NONE);
    header(h, "DRUMMACHINA", 0x1E, 0x04, 0xEE3C);              /* DMGo with another save size: neither */
    CHECK(lsdj_prog(h) == LSDJ_PROG_NONE);
    header(h, "LSDJ-ANY", 0x1B, 0x03, 0);                      /* LSDj's cartridge with a 32 KiB save: neither */
    CHECK(lsdj_prog(h) == LSDJ_PROG_NONE);
    header(h, "GAME", 0x19, 0x00, 0);                          /* MBC5, no RAM */
    CHECK(lsdj_prog(h) == LSDJ_PROG_NONE && !lsdj_prog_save(LSDJ_PROG_NONE));
    {   /* the header out of a bank table */
        static uint8_t bank0[0x150];
        const lsdj_bank_t t[1] = {{bank0, sizeof bank0}};
        uint8_t g[LSDJ_HDR];
        header(bank0 + 0x134, "DRUMMACHINA", 0x1E, 0x03, 0xEE3C);
        lsdj_header(t, 1, g);
        CHECK(!memcmp(g, bank0 + 0x134, LSDJ_HDR) && lsdj_prog(g) == LSDJ_PROG_DMGO);
        lsdj_header(t, 0, g);                                  /* no ROM: all 0xFF, nothing */
        CHECK(g[0] == 0xFF && lsdj_prog(g) == LSDJ_PROG_NONE);
    }
}

static void test_pad(void)
{
    /* the button bit of each Game Boy key, in JOYPAD bit order: A, B (no button: note keys), SELECT, START, RIGHT,
     * LEFT, UP, DOWN */
    const uint32_t map[8] = {0, 0, 1u << 11, 1u << 12, 1u << 10, 1u << 8, 1u << 3, 1u << 9};
    uint32_t i;
    CHECK(lsdj_pad(0, 0, map) == 0xFF);                               /* active low: nothing held */
    for (i = 2; i < 8u; i++)
        CHECK(lsdj_pad(map[i], 0, map) == (uint8_t)~(1u << i));       /* each button: its key only */
    CHECK(LSDJ_NOTE_A == 1u << 24 && LSDJ_NOTE_B == 1u << 26);        /* note bit 0 = F3: F5 = 24, G5 = 26 */
    CHECK(lsdj_pad(0, LSDJ_NOTE_A, map) == (uint8_t)~JOYPAD_A);       /* the top F of the keyboard: A */
    CHECK(lsdj_pad(0, LSDJ_NOTE_B, map) == (uint8_t)~JOYPAD_B);       /* the top G: B */
    CHECK(lsdj_pad(0, 1u << 25, map) == 0xFF);                        /* F#5 between them: no key */
    CHECK(lsdj_pad(0, 0x00FFFFFFu, map) == 0xFF);                     /* every note key below: no key */
    CHECK(lsdj_pad(map[2] | map[4], LSDJ_NOTE_A, map) == (uint8_t)~(JOYPAD_SELECT | JOYPAD_RIGHT | JOYPAD_A));
    CHECK(lsdj_pad(0xFFFFFFFFu, 0xFFFFFFFFu, map) == 0x00);
    CHECK(lsdj_pad((1u << 0) | (1u << 1) | (1u << 2), 0, map) == 0xFF);   /* OCT- / OCT+ / others: no Game Boy key */
    /* DMGo's: A and B on D#5 and F#5, the white keys free for the steps */
    CHECK(DMGO_NOTE_A == 1u << 22 && DMGO_NOTE_B == 1u << 25);
    CHECK(lsdj_pad_ab(0, DMGO_NOTE_A, map, DMGO_NOTE_A, DMGO_NOTE_B) == (uint8_t)~JOYPAD_A);
    CHECK(lsdj_pad_ab(0, DMGO_NOTE_B, map, DMGO_NOTE_A, DMGO_NOTE_B) == (uint8_t)~JOYPAD_B);
    CHECK(lsdj_pad_ab(0, LSDJ_NOTE_A | LSDJ_NOTE_B, map, DMGO_NOTE_A, DMGO_NOTE_B) == 0xFF);   /* F5, G5: steps 15, 16 */
    {
        uint32_t i, white = 0;
        for (i = 0; i < 16u; i++) {
            CHECK(i == 0 || LSDJ_STEP_NOTE[i] > LSDJ_STEP_NOTE[i - 1u]);   /* left to right */
            white |= LSDJ_STEP_NOTE[i];
        }
        CHECK(white == 0x5AD5AD5u);   /* F3 G3 A3 B3 C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5 G5: the 16 white keys */
        CHECK(!(white & (DMGO_NOTE_A | DMGO_NOTE_B)));
        CHECK(lsdj_pad_ab(0, white, map, DMGO_NOTE_A, DMGO_NOTE_B) == 0xFF);   /* steps are no Game Boy key */
    }
}

static uint32_t presses(lsdj_knobs_t *k, uint8_t key, uint32_t frames)   /* times `key` goes down in that many frames */
{
    uint32_t n = 0, was = 0, i;
    for (i = 0; i < frames; i++) {
        uint32_t now = lsdj_knobs_frame(k) & key;
        n += now && !was;
        was = now;
    }
    return n;
}

static void test_knobs(void)
{
    lsdj_knobs_t k;
    memset(&k, 0, sizeof k);
    CHECK(lsdj_knobs_frame(&k) == 0);                                 /* nothing turned: no key */
    lsdj_knobs_add(&k, LSDJ_AX_LR, 2);                                /* two steps right: down, up, down, up */
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_RIGHT && lsdj_knobs_frame(&k) == 0);
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_RIGHT && lsdj_knobs_frame(&k) == 0);
    CHECK(lsdj_knobs_frame(&k) == 0);
    lsdj_knobs_add(&k, LSDJ_AX_LR, -1);
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_LEFT && lsdj_knobs_frame(&k) == 0);
    lsdj_knobs_add(&k, LSDJ_AX_UD, 1);                                /* + = down */
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_DOWN && lsdj_knobs_frame(&k) == 0);
    lsdj_knobs_add(&k, LSDJ_AX_UD, -1);
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_UP && lsdj_knobs_frame(&k) == 0);
    /* the screen shortcut: SELECT one frame before the direction (LSDj needs that) and one after */
    lsdj_knobs_add(&k, LSDJ_AX_NAV_LR, 1);                            /* SELECT down two frames before the direction: */
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_SELECT && lsdj_knobs_frame(&k) == JOYPAD_SELECT);   /* (LSDj's PROJECT screen) */
    CHECK(lsdj_knobs_frame(&k) == (JOYPAD_SELECT | JOYPAD_RIGHT));
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_SELECT && lsdj_knobs_frame(&k) == 0);
    lsdj_knobs_add(&k, LSDJ_AX_NAV_UD, -1);
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_SELECT && lsdj_knobs_frame(&k) == JOYPAD_SELECT);
    CHECK(lsdj_knobs_frame(&k) == (JOYPAD_SELECT | JOYPAD_UP));
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_SELECT && lsdj_knobs_frame(&k) == 0);
    lsdj_knobs_add(&k, LSDJ_AX_LR, 3);                                /* turned back before it was sent: nothing */
    lsdj_knobs_add(&k, LSDJ_AX_LR, -3);
    CHECK(lsdj_knobs_frame(&k) == 0);
    lsdj_knobs_add(&k, LSDJ_AX_LR, 1000);                             /* a spun knob does not run on */
    CHECK(presses(&k, JOYPAD_RIGHT, 200) == LSDJ_KNOB_MAX);
    lsdj_knobs_add(&k, LSDJ_AX_UD, -1000);
    CHECK(presses(&k, JOYPAD_UP, 200) == LSDJ_KNOB_MAX);
    memset(&k, 0, sizeof k);                                          /* two knobs at once take turns; no press is cut */
    lsdj_knobs_add(&k, LSDJ_AX_LR, 2);
    lsdj_knobs_add(&k, LSDJ_AX_NAV_UD, 1);
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_RIGHT && lsdj_knobs_frame(&k) == 0);
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_SELECT && lsdj_knobs_frame(&k) == JOYPAD_SELECT);
    CHECK(lsdj_knobs_frame(&k) == (JOYPAD_SELECT | JOYPAD_DOWN));
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_SELECT && lsdj_knobs_frame(&k) == 0);
    CHECK(lsdj_knobs_frame(&k) == JOYPAD_RIGHT && lsdj_knobs_frame(&k) == 0 && lsdj_knobs_frame(&k) == 0);
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
    CHECK(lsdj_line(fb[5], px) == 1);                         /* changed: reported */
    CHECK(fb[5][0] == 0 && fb[5][1] == 1 && fb[5][3] == 3 && fb[5][159] == 3);
    CHECK(lsdj_line(fb[5], px) == 0);                         /* the same pixels again: not changed */
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

static void test_master(void)
{
    /* the MASTER pot (10-bit ADC) -> gain Q12, as Felucca: an IIR on the reading, then squared (an audio taper) */
    int32_t knob = 0, out[4];
    uint32_t i, q = 0, prev = 0, mono = 1;
    for (i = 0; i < 100; i++)
        q = lsdj_master(&knob, 1023);
    CHECK(q > 4000 && q <= 4096);                             /* full: ~1.0 */
    for (i = 0; i < 100; i++)
        q = lsdj_master(&knob, 0);
    CHECK(q == 0);                                            /* off: silence */
    for (i = 0; i < 100; i++)
        q = lsdj_master(&knob, 512);
    CHECK(q > 900 && q < 1100);                               /* half way: ~1/4 (squared) */
    knob = 0;
    for (i = 0; i < 40; i++) {                                /* a turn up is smooth: rises, never jumps */
        q = lsdj_master(&knob, 1023);
        mono &= q >= prev && q - prev < 1200u;
        prev = q;
    }
    CHECK(mono);
    CHECK(lsdj_master(&knob, -1) == q);                       /* an ADC timeout keeps the last gain */
    out[0] = 32767; out[1] = -32768; out[2] = 1000; out[3] = -1000;
    lsdj_gain(out, 2, 4096);
    CHECK(out[0] == 32767 && out[1] == -32768 && out[2] == 1000);
    lsdj_gain(out, 2, 2048);
    CHECK(out[0] == 16383 && out[1] == -16384 && out[2] == 500 && out[3] == -500);
    lsdj_gain(out, 2, 0);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
}

static void test_speed(void)
{
    /* speed % against the Game Boy's own frame rate (4194304 / 70224 = 59.7275 Hz), over a window in ms */
    CHECK(lsdj_speed(239, 4000) == 100);                      /* 59.75 fps: real time */
    CHECK(lsdj_speed(59, 1000) == 99);                        /* whole frames in 1 s: 59 is 98.8 % ... */
    CHECK(lsdj_speed(60, 1000) == 100);                       /* ... 60 is 100.5 %: both round to ~100 */
    CHECK(lsdj_speed(215, 4000) == 90);                       /* 53.75 fps */
    CHECK(lsdj_speed(0, 4000) == 0);
    CHECK(lsdj_speed(10, 0) == 0);                            /* no window yet */
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

/* DMGo's step keys on a made-up work RAM (C000..DFFF): a channel page, the cursor somewhere else */
static uint8_t wram[0x2000];
static void dmgo_page(void)
{
    memset(wram, 0, sizeof wram);
    wram[0x02F] = 1;                          /* past the title */
    wram[0x01A] = 3;                          /* the cursor: field 3, column 5 */
    wram[0x01B] = wram[0xB1F] = 5;
}
#define CURSOR(f, c) (wram[0x01A] == (f) && wram[0x01B] == (c) && wram[0xB1F] == (c))

static void test_steps(void)
{
    lsdj_steps_t s;
    uint32_t i, n;
    memset(&s, 0, sizeof s);
    dmgo_page();
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF);            /* nothing pressed: the owner's keys */
    lsdj_steps_keys(&s, LSDJ_STEP_NOTE[2]);                    /* step 3 */
    CHECK(lsdj_steps_frame(&s, wram, (uint8_t)~JOYPAD_RIGHT) == (uint8_t)~JOYPAD_B);   /* B alone, the cursor on it */
    CHECK(CURSOR(12, 2));
    lsdj_steps_keys(&s, LSDJ_STEP_NOTE[2]);                    /* still held: no second press */
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));   /* B up, the cursor back */
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));   /* done */

    lsdj_steps_keys(&s, 0);                                    /* steps 1 and 16 together: one after the other */
    lsdj_steps_keys(&s, LSDJ_STEP_NOTE[0] | LSDJ_STEP_NOTE[15]);
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == (uint8_t)~JOYPAD_B && CURSOR(12, 0));
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == (uint8_t)~JOYPAD_B && CURSOR(12, 15));
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF);

    /* the owner holds B (F#5): the step waits until B has been up a whole frame (DMGo takes a press, not a hold) */
    lsdj_steps_keys(&s, 0);
    lsdj_steps_keys(&s, LSDJ_STEP_NOTE[4]);
    CHECK(lsdj_steps_frame(&s, wram, (uint8_t)~JOYPAD_B) == (uint8_t)~JOYPAD_B && CURSOR(3, 5));   /* (theirs) */
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));   /* B just up: DMGo saw it down last frame */
    CHECK(lsdj_steps_frame(&s, wram, 0xFF) == (uint8_t)~JOYPAD_B && CURSOR(12, 4));
    CHECK(lsdj_steps_frame(&s, wram, (uint8_t)~JOYPAD_B) == 0xFF && CURSOR(3, 5));   /* B up whatever they hold */

    /* not on a channel page with nothing over it: the step is dropped, nothing touched */
    {
        static const struct { uint32_t at; uint8_t v; } NOT[] = {
            {0x02F, 0},                       /* the title screen */
            {0x005, 4}, {0x005, 5},           /* PATTERNS, SAVE */
            {0x006, 1},                       /* the top menu open */
            {0xA34, 1}};                      /* PICK A PAT open */
        for (i = 0; i < sizeof NOT / sizeof NOT[0]; i++) {
            dmgo_page();
            wram[NOT[i].at] = NOT[i].v;
            lsdj_steps_keys(&s, 0);
            lsdj_steps_keys(&s, LSDJ_STEP_NOTE[1]);
            CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));
            wram[NOT[i].at] = NOT[i].v == 0 ? 1 : 0;           /* the page right again: the step was not kept */
            CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));
        }
        dmgo_page();
        wram[0x005] = 3;                      /* CH4: a channel page */
        lsdj_steps_keys(&s, 0);
        lsdj_steps_keys(&s, LSDJ_STEP_NOTE[1]);
        CHECK(lsdj_steps_frame(&s, wram, 0xFF) == (uint8_t)~JOYPAD_B && CURSOR(12, 1));
        CHECK(lsdj_steps_frame(&s, wram, 0xFF) == 0xFF && CURSOR(3, 5));
    }

    /* 16 wait at most: a key pressed 20 times before a frame runs toggles 16 times */
    dmgo_page();
    for (i = 0; i < 20u; i++) {
        lsdj_steps_keys(&s, 0);
        lsdj_steps_keys(&s, LSDJ_STEP_NOTE[7]);
    }
    for (i = n = 0; i < 60u; i++)
        n += lsdj_steps_frame(&s, wram, 0xFF) == (uint8_t)~JOYPAD_B;
    CHECK(n == 16 && CURSOR(3, 5));
}

int main(void)
{
    test_rom();
    test_prog();
    test_pad();
    test_steps();
    test_knobs();
    test_pad_text();
    test_line_and_span();
    test_scale();
    test_ring();
    test_unpack();
    test_master();
    test_speed();
    printf(fails ? "lsdj_gb_test: %d FAILED\n" : "lsdj_gb_test: all passed\n", fails);
    return fails != 0;
}
