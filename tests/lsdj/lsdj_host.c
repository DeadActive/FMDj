/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* Host harness of the LSDj unit's emulator (firmware/src/lsdj/lsdj_emu.c): runs the owner's own ROM, never stored
 * in this repo, for some frames with the keys held as given, and writes the save RAM, a screenshot and the sound.
 *   lsdj_host ROM FRAMES [-s SAVE_IN] [-o SAVE_OUT] [-p SHOT.pgm] [-w SOUND.wav] [-m MODE] [-k FROM:TO:KEYS]...
 *             [-W WRAM_OUT]
 * MODE: lsdj_emu.c's emu_mode bits (1 inline ROM read, 2 frame skip, 4 the wait loop skipped, 8 sound within the
 * frame; all on by default). Prints a hash of every frame's sound and machine (registers, counters, memory) and the
 * final save RAM: the speed options (1, 2, 4) must not change it.
 * KEYS: letters of l r u d a b s(elect) t(start). Exit 0 = the ROM ran, no emulator errors, the screen not blank.
 * tools/lsdj_rom.py runs it to make the start-up save (LSDj past its first-start check). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LSDJ_HOST 1
#include <time.h>
#include "lsdj/lsdj_gb.c"
#include "lsdj/lsdj_emu.c"

static uint8_t rom[64u * LSDJ_BANK], sram[LSDJ_SRAM], fb[LSDJ_H][LSDJ_W];
static lsdj_bank_t banks[64];

static uint8_t keys_at(uint32_t f, int argc, char **argv)
{
    uint32_t down = 0;
    int i;
    for (i = 1; i + 1 < argc; i++) {
        unsigned a, b;
        char k[16];
        if (strcmp(argv[i], "-k") || sscanf(argv[i + 1], "%u:%u:%15s", &a, &b, k) != 3 || f < a || f >= b)
            continue;
        for (char *c = k; *c; c++)
            down |= *c == 'l' ? JOYPAD_LEFT : *c == 'r' ? JOYPAD_RIGHT : *c == 'u' ? JOYPAD_UP :
                    *c == 'd' ? JOYPAD_DOWN : *c == 'a' ? JOYPAD_A : *c == 'b' ? JOYPAD_B :
                    *c == 's' ? JOYPAD_SELECT : *c == 't' ? JOYPAD_START : 0;
    }
    return (uint8_t)~down;
}

static const char *opt(int argc, char **argv, const char *o)
{
    int i;
    for (i = 3; i + 1 < argc; i++)
        if (!strcmp(argv[i], o))
            return argv[i + 1];
    return 0;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

/* what the next frame depends on (not the picture: frame skip). all: the memories too (every 64th frame: they are
 * 24 KiB, and a difference in them stays or shows in the registers) */
static uint32_t machine(uint32_t h, int all)
{
    const struct gb_s *g = &emu.gb;
    uint32_t r[] = {g->cpu_reg.a, g->cpu_reg.f.reg, g->cpu_reg.bc.reg, g->cpu_reg.de.reg, g->cpu_reg.hl.reg,
                    g->cpu_reg.sp.reg, g->cpu_reg.pc.reg, g->gb_halt, g->gb_ime,
                    (uint32_t)g->counter.lcd_count, (uint32_t)g->counter.div_count, (uint32_t)g->counter.tima_count,
                    (uint32_t)g->counter.serial_count, (uint32_t)g->counter.lcd_off_count};
    h = fnv(h, r, sizeof r);
    h = fnv(h, g->hram_io, sizeof g->hram_io);
    h = fnv(h, &emu_apu, sizeof emu_apu);
    if (!all)
        return h;
    h = fnv(h, g->wram, sizeof g->wram);
    h = fnv(h, g->vram, sizeof g->vram);
    return fnv(h, g->oam, sizeof g->oam);
}

static void wav_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }

int main(int argc, char **argv)
{
    FILE *f, *wav = 0;
    long n;
    uint32_t i, frames, nb, ink = 0, peak = 0, nsamp = 0, hash = 2166136261u;
    clock_t t0;
    int r;
    const char *p;
    if (argc < 3) {
        fprintf(stderr, "usage: lsdj_host ROM FRAMES [-s SAVE_IN] [-o SAVE_OUT] [-p SHOT.pgm] [-w SOUND.wav] "
                        "[-k FROM:TO:KEYS]...\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f)
        return perror(argv[1]), 2;
    n = (long)fread(rom, 1, sizeof rom, f);
    fclose(f);
    memset(rom + n, 0xFF, sizeof rom - (size_t)n);
    nb = (uint32_t)((n + LSDJ_BANK - 1) / LSDJ_BANK);
    for (i = 0; i < nb; i++) {                    /* the same trimming as tools/lsdj_rom.py */
        uint32_t k = LSDJ_BANK;
        while (k && rom[i * LSDJ_BANK + k - 1u] == 0xFF)
            k--;
        banks[i].p = rom + i * LSDJ_BANK;
        banks[i].n = (uint16_t)k;
    }
    memset(sram, 0, sizeof sram);
    if ((p = opt(argc, argv, "-s")) && (f = fopen(p, "rb"))) {
        fread(sram, 1, sizeof sram, f);
        fclose(f);
    }
    if ((p = opt(argc, argv, "-m")))
        emu_mode = (uint32_t)atoi(p);
    r = emu_init(banks, nb, sram, fb);
    if (r) {
        fprintf(stderr, "emu_init: %d\n", r);
        return 1;
    }
    if ((p = opt(argc, argv, "-w")) && (wav = fopen(p, "wb")))
        fseek(wav, 44, SEEK_SET);
    frames = (uint32_t)atoi(argv[2]);
    t0 = clock();
    for (i = 0; i < frames; i++) {
        uint32_t s;
        emu_frame(keys_at(i, argc, argv));
        for (s = 0; s < AUDIO_SAMPLES_TOTAL; s++)
            hash = (hash ^ (uint16_t)emu.snd[s]) * 16777619u;
        hash = machine(hash, i % 64u == 63u || i + 1u == frames);
        for (s = 0; s < AUDIO_SAMPLES_TOTAL; s++) {
            uint32_t a = (uint32_t)(emu.snd[s] < 0 ? -emu.snd[s] : emu.snd[s]);
            peak = a > peak ? a : peak;
        }
        if (wav)
            fwrite(emu.snd, sizeof emu.snd[0], AUDIO_SAMPLES_TOTAL, wav);
        nsamp += AUDIO_SAMPLES;
    }
    if (wav) {
        rewind(wav);
        fwrite("RIFF", 1, 4, wav); wav_u32(wav, 36 + nsamp * 4); fwrite("WAVEfmt ", 1, 8, wav);
        wav_u32(wav, 16); wav_u32(wav, 1 | 2u << 16); wav_u32(wav, 44100); wav_u32(wav, 44100 * 4);
        wav_u32(wav, 4 | 16u << 16); fwrite("data", 1, 4, wav); wav_u32(wav, nsamp * 4);
        fclose(wav);
    }
    for (i = 0; i < LSDJ_H * LSDJ_W; i++)
        ink += fb[0][i] != fb[0][0];
    if ((p = opt(argc, argv, "-p")) && (f = fopen(p, "wb"))) {
        fprintf(f, "P5 160 144 255\n");
        for (i = 0; i < LSDJ_H * LSDJ_W; i++)
            fputc(255 - fb[0][i] * 85, f);
        fclose(f);
    }
    if ((p = opt(argc, argv, "-W")) && (f = fopen(p, "wb"))) {   /* the work RAM, C000..DFFF (tests read it) */
        fwrite(emu.gb.wram, 1, sizeof emu.gb.wram, f);
        fclose(f);
    }
    if ((p = opt(argc, argv, "-o")) && (f = fopen(p, "wb"))) {
        fwrite(sram, 1, emu.sram_n, f);                /* the program's save: 128 KiB (LSDj), 32 KiB (DMGo) */
        fclose(f);
    }
    {
        double us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / (frames ? frames : 1);
        for (i = 0; i < LSDJ_SRAM; i++)
            hash = (hash ^ sram[i]) * 16777619u;
        printf("frames %u, emulator errors %u, ink %u px, audio peak %u, %u banks, mode %u, hash %08x, %.1f us/frame, "
               "wait loop: %u instructions a frame skipped\n",
               frames, emu.errors, ink, peak, nb, emu_mode, hash, us, frames ? emu.skipped / frames : 0);
    }
    return emu.errors || !ink;
}
