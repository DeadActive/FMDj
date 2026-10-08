/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* minigb_apu's "zombie mode" (firmware/src/lsdj/minigb_apu.c): a write to NRx2 while the channel plays changes its
 * volume as the hardware does (Pan Docs, Audio details, "Zombie mode"):
 *   1. old envelope period 0 and the envelope still running: volume + 1; else, old mode subtract: volume + 2
 *   2. the mode changed (add <-> subtract): volume = 16 - volume
 *   3. only the low 4 bits are kept
 * LSDj 9 builds its envelopes from such writes.
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

int main(void)
{
    int i, v;
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
