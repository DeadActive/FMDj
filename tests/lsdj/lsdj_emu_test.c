/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The emulator glue (firmware/src/lsdj/lsdj_emu.c) on a small Game Boy program of our own (assembled below by hand;
 * no LSDj data), which does what LSDj's sound engine does: a timer interrupt 358 times a second (6 in a Game Boy
 * frame, not in step with the frames), a note every 64th of them, and between interrupts a wait loop of the same
 * 8 instructions as LSDj's main loop. It also turns the screen off and on and runs a serial transfer now and
 * then (noting when its interrupt came), so every branch of Peanut-GB's timing code is met.
 *   1. EMU_SND_FINE: a note starts where in the frame its register write happened: the notes are evenly spaced.
 *      Without it they start at frame boundaries (16.7 ms apart): uneven.
 *   2. EMU_WAIT_SKIP: the wait loop is not run instruction by instruction, and nothing else changes: after every
 *      frame the whole machine (registers, counters, memory, picture, sound) is the same with and without it.
 *   3. emu_wait, one call at a time: from 300000 made-up states of the machine in the wait loop (any of its 8
 *      instructions, any counters, timer rate, screen mode, serial transfer; some with a flag set, an interrupt
 *      waiting or registers the loop would not have), a call that skips leaves exactly the machine that running the
 *      skipped instructions leaves, and a call that does not skip leaves it untouched.
 *   cc -O1 -w -Ifirmware/src -Ifirmware/src/lsdj -o build/tests/lsdj_emu_test tests/lsdj/lsdj_emu_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LSDJ_HOST 1
#include <time.h>
#include "lsdj/lsdj_gb.c"
#include "lsdj/lsdj_emu.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t rom[2u * LSDJ_BANK], sram[LSDJ_SRAM], fb[LSDJ_H][LSDJ_W];
static lsdj_bank_t banks[2];

static void put(uint32_t at, const uint8_t *b, uint32_t n) { memcpy(rom + at, b, n); }
#define AT(a, ...) do { static const uint8_t b_[] = {__VA_ARGS__}; put((a), b_, sizeof b_); } while (0)

/* the wait loop's flags: C100 set by the timer interrupt, C101 by the VBLANK interrupt; C102 counts timer
 * interrupts, C103 frames; C104, C105: when the last serial transfer ended */
static void make_rom(int wait_loop)
{
    uint32_t i;
    uint8_t x = 0;
    memset(rom, 0xFF, sizeof rom);
    AT(0x0040, 0xF5, 0x3E, 0x01, 0xEA, 0x01, 0xC1, 0xF1, 0xD9);          /* VBLANK: push af; ld a,1; ld (C101),a; pop af; reti */
    AT(0x0050, 0xF5, 0x3E, 0x01, 0xEA, 0x00, 0xC1, 0xF1, 0xD9);          /* timer: ... ld (C100),a ... */
    AT(0x0058, 0xF5, 0xF0, 0x05, 0xEA, 0x04, 0xC1, 0xF0, 0x44, 0xEA, 0x05, 0xC1, 0xF1, 0xD9);   /* serial: when it
                                               * came: C104 = TIMA, C105 = LY (the wait loop goes on) */
    AT(0x0100, 0x00, 0xC3, 0x50, 0x01);                                  /* nop; jp 0150 */
    memset(rom + 0x104, 0, 0x4C);                                        /* the header: MBC5 + RAM + battery; a 128 KiB save */
    rom[0x147] = 0x1B;
    rom[0x148] = 0x00;
    rom[0x149] = 0x04;
    for (i = 0x134; i <= 0x14C; i++)
        x = (uint8_t)(x - rom[i] - 1u);
    rom[0x14D] = x;
    AT(0x0150,
       0xF3,                                  /* di */
       0x31, 0xFF, 0xDF,                      /* ld sp,DFFF */
       0x3E, 0x80, 0xE0, 0x26,                /* NR52 = 80: sound on */
       0x3E, 0x77, 0xE0, 0x24,                /* NR50 = 77 */
       0x3E, 0xFF, 0xE0, 0x25,                /* NR51 = FF */
       0x3E, 0xB0, 0xE0, 0x11,                /* NR11 = B0: duty 50 %, length 16 / 256 s */
       0x3E, 0xF0, 0xE0, 0x12,                /* NR12 = F0: volume 15, no envelope */
       0x3E, 0x83, 0xE0, 0x13,                /* NR13 = 83 */
       0x3E, 0x49, 0xE0, 0x06, 0xE0, 0x05,    /* TMA = TIMA = 49: 183 counts */
       0x3E, 0x06, 0xE0, 0x07,                /* TAC = 06: on, 65536 Hz: an interrupt every 11712 cycles */
       0xAF, 0xE0, 0x0F,                      /* IF = 0 */
       0xEA, 0x00, 0xC1, 0xEA, 0x01, 0xC1, 0xEA, 0x02, 0xC1, 0xEA, 0x03, 0xC1,   /* C100..C103 = 0 */
       0x3E, 0x0D, 0xE0, 0xFF,                /* IE = 0D: VBLANK, timer, serial */
       0xFB,                                  /* ei */
       0xC3, 0x00, 0x02);                     /* jp 0200 */
    if (wait_loop)
        AT(0x0200,
           0xAF,                              /* 0200 xor a */
           0x21, 0x00, 0xC1,                  /* 0201 ld hl,C100 */
           0xB6,                              /* 0204 or (hl) */
           0xC2, 0x40, 0x02,                  /* 0205 jp nz,0240 */
           0xAF,                              /* 0208 xor a */
           0x21, 0x01, 0xC1,                  /* 0209 ld hl,C101 */
           0xB6,                              /* 020C or (hl) */
           0xCA, 0x00, 0x02);                 /* 020D jp z,0200 */
    else                                      /* the same wait with other instructions: nothing to recognise */
        AT(0x0200,
           0xFA, 0x00, 0xC1,                  /* 0200 ld a,(C100) */
           0xB7,                              /* 0203 or a */
           0xC2, 0x40, 0x02,                  /* 0204 jp nz,0240 */
           0xFA, 0x01, 0xC1,                  /* 0207 ld a,(C101) */
           0xB7,                              /* 020A or a */
           0xCA, 0x00, 0x02,                  /* 020B jp z,0200 */
           0x00, 0x00);                       /* 020E nop; nop */
    AT(0x0210,                                /* a frame went by (0210 .. 0224): */
       0xAF, 0xEA, 0x01, 0xC1,                /* C101 = 0 */
       0x21, 0x03, 0xC1, 0x34,                /* inc (C103) */
       0x7E, 0xFE, 0x64,                      /* ld a,(hl); cp 100 */
       0xC2, 0x00, 0x02,                      /* jp nz,0200 */
       0x3E, 0x11, 0xE0, 0x40,                /* the 100th: LCDC = 11: the screen off */
       0xC3, 0x00, 0x02);
    AT(0x0240,                                /* a timer interrupt went by: */
       0xAF, 0xEA, 0x00, 0xC1,                /* 0240 C100 = 0 */
       0x21, 0x02, 0xC1, 0x34,                /* 0244 inc (C102) */
       0xC2, 0x54, 0x02,                      /* 0248 jp nz,0254 */
       0x3E, 0x91, 0xE0, 0x40,                /* 024B every 256th: LCDC = 91: the screen on, */
       0x3E, 0x81, 0xE0, 0x02,                /* 024F and SC = 81: a serial transfer */
       0x00,                                  /* 0253 nop */
       0x7E, 0xE6, 0x3F,                      /* 0254 ld a,(hl); and 3F */
       0xC2, 0x00, 0x02,                      /* 0257 jp nz,0200 */
       0x3E, 0xC6, 0xE0, 0x14,                /* 025A every 64th: NR14 = C6: the note (trigger, length on) */
       0xC3, 0x00, 0x02);                     /* 025E jp 0200 */
    banks[0].p = rom;
    banks[0].n = LSDJ_BANK;
    banks[1].p = rom + LSDJ_BANK;
    banks[1].n = 0;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static uint32_t machine(uint32_t h)                    /* everything a later frame could depend on */
{
    const struct gb_s *g = &emu.gb;
    uint32_t r[] = {g->cpu_reg.a, g->cpu_reg.f.reg, g->cpu_reg.bc.reg, g->cpu_reg.de.reg, g->cpu_reg.hl.reg,
                    g->cpu_reg.sp.reg, g->cpu_reg.pc.reg, g->gb_halt, g->gb_ime, g->gb_frame,
                    (uint32_t)g->counter.lcd_count, (uint32_t)g->counter.div_count, (uint32_t)g->counter.tima_count,
                    (uint32_t)g->counter.serial_count, (uint32_t)g->counter.lcd_off_count};
    h = fnv(h, r, sizeof r);
    h = fnv(h, g->wram, sizeof g->wram);
    h = fnv(h, g->vram, sizeof g->vram);
    h = fnv(h, g->oam, sizeof g->oam);
    h = fnv(h, g->hram_io, sizeof g->hram_io);
    h = fnv(h, &emu_apu, sizeof emu_apu);
    h = fnv(h, fb, sizeof fb);
    return fnv(h, emu.snd, sizeof emu.snd);
}

#define FRAMES 700u
static uint32_t run(uint32_t mode, uint32_t *hashes, int16_t *sound)
{
    uint32_t f;
    emu_mode = mode;
    memset(sram, 0, sizeof sram);
    memset(fb, 0, sizeof fb);
    CHECK(emu_init(banks, 2, sram, fb) == 0);
    for (f = 0; f < FRAMES; f++) {
        emu_frame(0xFF);
        if (hashes)
            hashes[f] = machine(2166136261u);
        if (sound)
            memcpy(sound + f * AUDIO_SAMPLES_TOTAL, emu.snd, sizeof emu.snd);
    }
    CHECK(emu.errors == 0);
    CHECK(emu.gb.wram[0x103] != 0 && emu.gb.wram[0x102] != 0);        /* (the program ran: both counters moved) */
    return emu.skipped;
}

/* where the notes start (stereo frame numbers): sound after >= 1000 silent frames */
static uint32_t onsets(const int16_t *s, uint32_t *at, uint32_t max)
{
    uint32_t i, quiet = 1000, n = 0;
    for (i = 0; i < FRAMES * AUDIO_SAMPLES; i++) {
        if (s[2u * i] | s[2u * i + 1u]) {
            if (quiet >= 1000u && n < max)
                at[n++] = i;
            quiet = 0;
        } else {
            quiet++;
        }
    }
    return n;
}

/* the gaps between the notes: the shortest and longest, and how many are within 2 samples of `mid` */
static uint32_t spread(const uint32_t *at, uint32_t n, uint32_t mid, uint32_t *lo, uint32_t *hi)
{
    uint32_t i, even = 0;
    for (*lo = ~0u, *hi = 0, i = 1; i < n; i++) {
        uint32_t d = at[i] - at[i - 1u];
        *lo = d < *lo ? d : *lo;
        *hi = d > *hi ? d : *hi;
        even += d + 2u >= mid && d <= mid + 2u;
    }
    return even;
}

static void test_sound_timing(void)
{
    static int16_t coarse[FRAMES * AUDIO_SAMPLES_TOTAL], fine[FRAMES * AUDIO_SAMPLES_TOTAL];
    uint32_t a[64], b[64], na, nb, lo, hi, i, even, on_frame = 0;
    make_rom(1);
    run(EMU_FAST_ROM, 0, coarse);
    run(EMU_FAST_ROM | EMU_SND_FINE, 0, fine);
    na = onsets(coarse, a, 64);
    nb = onsets(fine, b, 64);
    CHECK(na >= 40 && nb == na);                       /* a note every 64 interrupts: 178.7 ms, 7881 samples */
    for (i = 0; i < na; i++)
        on_frame += a[i] % AUDIO_SAMPLES == 0;
    CHECK(on_frame == na);                             /* per frame: every note starts on a frame boundary, */
    even = spread(a, na, 7872, &lo, &hi);
    CHECK(hi - lo >= AUDIO_SAMPLES && even == 0);      /* so the notes are a frame (16.7 ms) uneven */
    even = spread(b, nb, 7872, &lo, &hi);
    printf("  notes: %u; fine: %u .. %u samples apart, %u of %u gaps within 2 samples of 7872\n", nb, lo, hi, even,
           nb - 1u);
    CHECK(even >= nb - 1u - 4u);                       /* fine: evenly spaced, to 0.05 ms (the timer's 11712 cycles x
                                                        * 64 = 7872 samples of the frames Peanut-GB counts), */
    CHECK(hi - lo <= 24u);                             /* but for the few around the screen going off and on, where
                                                        * Peanut-GB's frame is not a frame long (0.5 ms) */
    for (i = 0; i < na && i < nb; i++)                 /* and each within the frame the old sound gave it to */
        CHECK(b[i] >= a[i] && b[i] < a[i] + AUDIO_SAMPLES);
}

static void test_wait_skip(void)
{
    static uint32_t h0[FRAMES], h1[FRAMES];
    uint32_t f, s0, s1, same = 0;
    for (f = 0; f < 2u; f++) {                         /* with both kinds of sound */
        uint32_t m = EMU_FAST_ROM | (f ? EMU_SND_FINE : 0);
        make_rom(1);
        s0 = run(m, h0, 0);
        s1 = run(m | EMU_WAIT_SKIP, h1, 0);
        CHECK(s0 == 0);
        CHECK(s1 > FRAMES * 5000u);                    /* of ~7400 instructions a frame, most are the loop's: skipped */
        CHECK(!memcmp(h0, h1, sizeof h0));             /* and after every frame the machine is the same */
        for (same = 0; same < FRAMES && h0[same] == h1[same]; same++)
            ;
        if (same < FRAMES)
            printf("  first difference: frame %u\n", same);
    }
    make_rom(0);                                       /* another program's wait: left alone */
    CHECK(run(EMU_FAST_ROM | EMU_WAIT_SKIP, 0, 0) == 0);
}

static uint32_t rnd_s = 12345u;
static uint32_t rnd(uint32_t n)
{
    rnd_s = rnd_s * 1664525u + 1013904223u;
    return (rnd_s >> 8) % n;
}

static void test_wait_calls(void)
{
    static const uint8_t SH[4] = {10, 4, 6, 8};
    static struct gb_s before, after;
    struct gb_s *g = &emu.gb;
    uint32_t t, k, n, i, m, skips = 0, turns = 0, bad = 0, touched = 0;
    make_rom(1);
    run(EMU_FAST_ROM | EMU_WAIT_SKIP, 0, 0);          /* a machine with the loop found */
    for (t = 0; t < 300000u; t++) {
        i = rnd(8);
        g->cpu_reg.pc.reg = (uint16_t)(0x200u + EMU_WAIT_AT[i]);
        g->cpu_reg.a = rnd(24) ? 0 : 1;
        g->cpu_reg.f.reg = rnd(24) ? 0x80 : (uint8_t)(rnd(16) << 4);
        g->cpu_reg.hl.reg = (uint16_t)(rnd(24) ? (i - 2u < 4u ? 0xC100u : 0xC101u) : 0xC100u + rnd(2));
        g->wram[0x100] = !rnd(40);
        g->wram[0x101] = !rnd(40);
        g->gb_halt = 0;
        g->gb_ime = rnd(8) != 0;
        g->hram_io[IO_IE] = rnd(4) ? 0x05 : (uint8_t)rnd(32);
        g->hram_io[IO_IF] = rnd(6) ? 0xE0 : (uint8_t)(0xE0u | rnd(32));
        g->hram_io[IO_TAC] = (uint8_t)(0xF8u | rnd(8));
        g->hram_io[IO_TIMA] = (uint8_t)(rnd(3) ? rnd(256) : 0xFFu);
        g->hram_io[IO_TMA] = (uint8_t)rnd(256);
        g->counter.tima_count = rnd(1u << SH[g->hram_io[IO_TAC] & 3u]);
        g->hram_io[IO_DIV] = (uint8_t)rnd(256);
        g->counter.div_count = rnd(256);
        g->hram_io[IO_SC] = (uint8_t)(rnd(3) ? 0x7Eu : 0x80u | rnd(2));
        g->counter.serial_count = rnd(3) ? rnd(4096) : 4095u - rnd(100);
        if (rnd(5)) {                                 /* the screen on: a line, a mode, a place in the line */
            m = rnd(4);
            g->hram_io[IO_LCDC] = 0x91;
            g->hram_io[IO_STAT] = (uint8_t)((g->hram_io[IO_STAT] & ~STAT_MODE) | m);
            g->hram_io[IO_LY] = (uint8_t)(m == IO_STAT_MODE_VBLANK ? 144u + rnd(10) : rnd(144));
            g->counter.lcd_count = !rnd(10) ? rnd(456) : m == IO_STAT_MODE_OAM_SCAN ? rnd(80) :
                                   m == IO_STAT_MODE_LCD_DRAW ? 80u + rnd(172) : rnd(456);
            g->counter.lcd_off_count = 0;
        } else {                                      /* off: mostly near the end of Peanut-GB's frame count */
            g->hram_io[IO_LCDC] = 0x11;
            g->counter.lcd_count = 0;
            g->counter.lcd_off_count = rnd(8) ? LCD_FRAME_CYCLES - 1u - rnd(1500) : rnd(LCD_FRAME_CYCLES);
        }
        memcpy(&before, g, sizeof before);
        k = emu.skipped;
        if (!emu_wait(g)) {
            touched += memcmp(&before, g, sizeof before) != 0;
            continue;
        }
        n = emu.skipped - k;
        memcpy(&after, g, sizeof after);
        memcpy(g, &before, sizeof before);
        for (k = 0; k < n; k++)                       /* the same instructions, run */
            __gb_step_cpu(g);
        bad += memcmp(&after, g, sizeof after) != 0;
        skips++;
        turns += n >= 8u;
    }
    printf("  wait calls: %u of %u skipped (%u a whole turn or more)\n", skips, t, turns);
    CHECK(bad == 0);
    CHECK(touched == 0);
    CHECK(skips > t / 3u && turns > t / 20u && skips < t);
    CHECK(emu.errors == 0);
}

int main(void)
{
    test_sound_timing();
    test_wait_skip();
    test_wait_calls();
    printf(fails ? "lsdj_emu_test: %d FAILED\n" : "lsdj_emu_test: all passed\n", fails);
    return fails != 0;
}
