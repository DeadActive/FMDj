/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The Game Boy: Peanut-GB (MIT, Mahyar Koshkouei) and minigb_apu (MIT, Alex Baines / Mahyar Koshkouei), glued to
 * the ROM bank table, the save RAM and a frame buffer. Shared by the firmware (lsdj.c) and the host harness
 * (tests/lsdj/lsdj_host.c). Include lsdj_gb.c first. Integer only (minigb_apu's float constants fold at compile
 * time). Freestanding: no libc headers beyond <stdint.h>, <stdbool.h>, <stddef.h>. */
#include <stddef.h>
#ifndef assert
#define assert(x) ((void)0)
#endif
#ifndef abort
#define abort() for (;;)
#endif
#ifndef LSDJ_HOST
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
#endif

#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#define AUDIO_SAMPLE_RATE 44100
#include "minigb_apu.c"
static struct minigb_apu_ctx emu_apu;
uint8_t audio_read(const uint16_t a) { return minigb_apu_audio_read(&emu_apu, a); }
void audio_write(const uint16_t a, const uint8_t v) { minigb_apu_audio_write(&emu_apu, a, v); }
/* speed options, switchable while running (lsdj.c: FX / LFO): the inline ROM read (peanut_gb.h PGB_FAST_ROM) and
 * drawing every 2nd frame only (Peanut-GB's frame skip) */
#define EMU_FAST_ROM 1u
#define EMU_FRAME_SKIP 2u
static uint32_t emu_mode = EMU_FAST_ROM | EMU_FRAME_SKIP;   /* both on at start */
static const lsdj_bank_t *emu_rom_t;
static uint32_t emu_rom_n;
#define PGB_FAST_ROM_ON (emu_mode & EMU_FAST_ROM)
#define PGB_FAST_ROM(off) lsdj_rom_byte(emu_rom_t, emu_rom_n, (uint32_t)(off))
/* the opcode / operand fetch: bank 0 and the selected bank (cached until LSDj switches) read directly. MBC5 only
 * (emu_init refuses other cartridges): 0x4000..0x7FFF is bank `bank`, bank 0 included */
static const uint8_t *emu_lo_p, *emu_hi_p;
static uint32_t emu_lo_n, emu_hi_n, emu_hi_bank;
static void emu_hi_set(uint32_t b)
{
    emu_hi_bank = b;
    emu_hi_p = b < emu_rom_n ? emu_rom_t[b].p : 0;
    emu_hi_n = b < emu_rom_n ? emu_rom_t[b].n : 0;
}
static inline __attribute__((always_inline)) int emu_fetch(uint32_t a, uint32_t bank, uint32_t boot)
{
    if (!(emu_mode & EMU_FAST_ROM))
        return -1;
    if (a < 0x4000u) {
        if (a < 0x100u && !boot)
            return -1;                        /* (a boot ROM: none here; Peanut-GB's own path) */
        return a < emu_lo_n ? emu_lo_p[a] : 0xFF;
    }
    if (a >= 0x8000u)
        return -1;
    if (bank != emu_hi_bank)
        emu_hi_set(bank);
    a -= 0x4000u;
    return a < emu_hi_n ? emu_hi_p[a] : 0xFF;
}
#define PGB_FAST_FETCH(a, bank, boot) emu_fetch((a), (bank), (boot))
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PEANUT_GB_USE_INTRINSICS 0
#include "peanut_gb.h"

#ifndef LSDJ_POOL
#define LSDJ_POOL                             /* the firmware: section(".pool") */
#endif
#define LSDJ_SRAM (128u * 1024u)
#define LSDJ_FRAME_SAMPLES AUDIO_SAMPLES      /* stereo frames per Game Boy frame (738 at 44.1 kHz) */

static struct {
    struct gb_s gb;
    const lsdj_bank_t *rom;
    uint32_t nbanks;
    uint8_t *sram;                            /* LSDJ_SRAM bytes */
    uint8_t (*fb)[LSDJ_W];                    /* LSDJ_H rows of shades */
    uint8_t *dirty;                           /* LSDJ_H flags: set when a row's pixels change (0: not kept) */
    uint32_t errors, last_error, last_addr;
    int16_t snd[AUDIO_SAMPLES_TOTAL];         /* the last frame's audio, interleaved stereo */
} emu LSDJ_POOL;

static uint8_t emu_rom_read(struct gb_s *gb, const uint_fast32_t a)
{
    (void)gb;
    return lsdj_rom_byte(emu_rom_t, emu_rom_n, (uint32_t)a);
}
static uint8_t emu_ram_read(struct gb_s *gb, const uint_fast32_t a)
{
    (void)gb;
    return a < LSDJ_SRAM ? emu.sram[a] : 0xFFu;
}
static void emu_ram_write(struct gb_s *gb, const uint_fast32_t a, const uint8_t v)
{
    (void)gb;
    if (a < LSDJ_SRAM)
        emu.sram[a] = v;
}
static void emu_error(struct gb_s *gb, const enum gb_error_e e, const uint16_t a)
{
    (void)gb;
    emu.errors++;
    emu.last_error = (uint32_t)e;
    emu.last_addr = a;
}
static void emu_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t y)
{
    (void)gb;
    if (y < LSDJ_H && lsdj_line(emu.fb[y], px) && emu.dirty)
        emu.dirty[y] = 1;
}

/* 0 = running; else Peanut-GB's gb_init error (bad header checksum, unsupported cartridge) or -1 (not MBC5 + RAM +
 * battery with a 128 KiB save: what LSDj is) */
static int emu_init(const lsdj_bank_t *rom, uint32_t nbanks, uint8_t *sram, uint8_t (*fb)[LSDJ_W])
{
    enum gb_init_error_e r;
    emu.rom = emu_rom_t = rom;
    emu.nbanks = emu_rom_n = nbanks;
    emu_lo_p = nbanks ? rom[0].p : 0;
    emu_lo_n = nbanks ? rom[0].n : 0;
    emu_hi_set(~0u);                          /* no bank cached yet */
    emu.sram = sram;
    emu.fb = fb;
    emu.dirty = 0;
    emu.errors = 0;
    if (lsdj_rom_byte(rom, nbanks, 0x147) != 0x1Bu || lsdj_rom_byte(rom, nbanks, 0x149) != 0x04u)
        return -1;
    r = gb_init(&emu.gb, emu_rom_read, emu_ram_read, emu_ram_write, emu_error, 0);
    if (r != GB_INIT_NO_ERROR)
        return (int)r;
    gb_init_lcd(&emu.gb, emu_line);
    minigb_apu_audio_init(&emu_apu);
    return 0;
}

static void emu_frame(uint8_t joypad)          /* one Game Boy frame: the lines into fb, the audio into emu.snd */
{
    emu.gb.direct.joypad = joypad;
    emu.gb.direct.frame_skip = (emu_mode & EMU_FRAME_SKIP) != 0;
    gb_run_frame(&emu.gb);
    minigb_apu_audio_callback(&emu_apu, emu.snd);
}
