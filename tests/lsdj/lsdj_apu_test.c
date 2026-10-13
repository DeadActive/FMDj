/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* minigb_apu's "zombie mode" (firmware/src/lsdj/minigb_apu.c): a write to NRx2 while the channel plays changes its
 * volume as the hardware does (Pan Docs, Audio details, "Zombie mode"):
 *   1. old envelope period 0 and the envelope still running: volume + 1; else, old mode subtract: volume + 2
 *   2. the mode changed (add <-> subtract): volume = 16 - volume
 *   3. only the low 4 bits are kept
 * LSDj 9 builds its envelopes from such writes.
 * And the sound in pieces (minigb_apu_audio_render): LSDj changes the registers about 6 times in a Game Boy frame, so
 * the frame's sound is made piece by piece, each write taking effect where it happened (lsdj_emu.c). The pieces of
 * a frame are the same samples as the whole frame at once, and a write between two pieces is heard from there on.
 *   cc -O1 -w -Ifirmware/src/lsdj -o build/host/lsdj_apu_test tests/lsdj/lsdj_apu_test.c */
#include <stdio.h>
#include <string.h>
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#define AUDIO_SAMPLE_RATE 44100
#include "minigb_apu.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static struct minigb_apu_ctx apu;

static int start(uint8_t nr12)                 /* channel 1 triggered with this NR12; its volume */
{
    minigb_apu_audio_init(&apu);
    minigb_apu_audio_write(&apu, 0xFF26, 0x80);
    minigb_apu_audio_write(&apu, 0xFF12, nr12);
    minigb_apu_audio_write(&apu, 0xFF14, 0x80);
    return apu.chans[0].volume;
}
static int nr12(uint8_t v)
{
    minigb_apu_audio_write(&apu, 0xFF12, v);
    return apu.chans[0].volume;
}

static void all_four(void)                     /* every channel sounding, with envelopes and a length */
{
    static const uint8_t W[][2] = {
        {0x26, 0x80}, {0x24, 0x77}, {0x25, 0xFF},
        {0x10, 0x15}, {0x11, 0x80}, {0x12, 0xF3}, {0x13, 0x83}, {0x14, 0x86},          /* pulse 1: sweep, decay */
        {0x16, 0x45}, {0x17, 0x8A}, {0x18, 0x21}, {0x19, 0xC7},                        /* pulse 2: a length */
        {0x1A, 0x80}, {0x1B, 0x00}, {0x1C, 0x20}, {0x1D, 0xD6}, {0x1E, 0x86},          /* wave */
        {0x20, 0x00}, {0x21, 0xA2}, {0x22, 0x34}, {0x23, 0x80}};                       /* noise */
    unsigned i;
    minigb_apu_audio_init(&apu);
    for (i = 0; i < sizeof W / sizeof W[0]; i++)
        minigb_apu_audio_write(&apu, 0xFF00 + W[i][0], W[i][1]);
}

static void test_pieces(void)
{
    static audio_sample_t whole[4][AUDIO_SAMPLES_TOTAL], parts[4][AUDIO_SAMPLES_TOTAL];
    static const unsigned CUT[] = {1, 7, 100, 0, 255, 300};      /* then the rest of the frame */
    unsigned f, i, at, loud = 0;
    all_four();
    for (f = 0; f < 4; f++)
        minigb_apu_audio_callback(&apu, whole[f]);
    all_four();
    for (f = 0; f < 4; f++) {
        for (i = 0, at = 0; i < sizeof CUT / sizeof CUT[0]; at += CUT[i++])
            minigb_apu_audio_render(&apu, parts[f] + 2 * at, CUT[i]);
        minigb_apu_audio_render(&apu, parts[f] + 2 * at, AUDIO_SAMPLES - at);
    }
    CHECK(!memcmp(whole, parts, sizeof whole));        /* the pieces of a frame = the frame at once */
    for (i = 0; i < AUDIO_SAMPLES_TOTAL; i++)
        loud += whole[0][i] != 0;
    CHECK(loud > AUDIO_SAMPLES);                       /* (and it is sound, not silence) */

    {   /* a note started after 300 samples of a frame: silence before it, sound after */
        static audio_sample_t b[AUDIO_SAMPLES_TOTAL];
        unsigned before = 0, after = 0;
        minigb_apu_audio_init(&apu);
        minigb_apu_audio_write(&apu, 0xFF26, 0x80);
        minigb_apu_audio_write(&apu, 0xFF24, 0x77);
        minigb_apu_audio_write(&apu, 0xFF25, 0xFF);
        minigb_apu_audio_write(&apu, 0xFF11, 0x80);
        minigb_apu_audio_write(&apu, 0xFF12, 0xF0);
        minigb_apu_audio_write(&apu, 0xFF13, 0x83);
        minigb_apu_audio_render(&apu, b, 300);
        minigb_apu_audio_write(&apu, 0xFF14, 0x86);    /* the trigger */
        minigb_apu_audio_render(&apu, b + 600, AUDIO_SAMPLES - 300);
        for (i = 0; i < 600; i++)
            before += b[i] != 0;
        for (i = 600; i < AUDIO_SAMPLES_TOTAL; i++)
            after += b[i] != 0;
        CHECK(before == 0);
        CHECK(after > AUDIO_SAMPLES - 300);
    }
}

int main(void)
{
    int i, v;
    test_pieces();
    CHECK(start(0x58) == 5);                   /* volume 5, add, period 0 */
    CHECK(nr12(0x58) == 6);                    /* rule 1: period 0, running: + 1 */
    CHECK(nr12(0x08) == 7);                    /* (the new volume nibble does not matter) */
    for (i = 0, v = 7; i < 9; i++)
        v = nr12(0x08);
    CHECK(v == 0);                             /* 16 -> 0: the low 4 bits */
    CHECK(start(0xF9) == 15);                  /* volume 15, add, period 1 */
    CHECK(nr12(0xF9) == 15);                   /* old period != 0, old mode add, same mode: unchanged */
    CHECK(start(0xA1) == 10);                  /* volume 10, subtract, period 1 */
    CHECK(nr12(0xA1) == 12);                   /* old mode subtract: + 2 */
    CHECK(start(0x58) == 5);                   /* add, period 0 ... */
    CHECK(nr12(0x50) == 10);                   /* ... to subtract: + 1 (rule 1), then 16 - 6 */
    CHECK(start(0xA1) == 10);                  /* subtract, period 1 ... */
    CHECK(nr12(0xA9) == 4);                    /* ... to add: + 2, then 16 - 12 */
    printf(fails ? "lsdj_apu_test: %d FAILED\n" : "lsdj_apu_test: all passed\n", fails);
    return fails != 0;
}
