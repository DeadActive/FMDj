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
uint8_t audio_read(const uint16_t a);         /* Peanut-GB's sound hooks: below, after the Game Boy itself */
void audio_write(const uint16_t a, const uint8_t v);
/* options, switchable while running (lsdj.c: FX, LFO, EDIT, ENV):
 *   speed: the inline ROM read (peanut_gb.h PGB_FAST_ROM), drawing every 2nd frame only (Peanut-GB's frame skip),
 *          LSDj's wait loop not run instruction by instruction (emu_wait);
 *   sound: a register write is heard where in the Game Boy frame it happened (emu_snd_to), not from the frame's
 *          start */
#define EMU_FAST_ROM 1u
#define EMU_FRAME_SKIP 2u
#define EMU_WAIT_SKIP 4u
#define EMU_SND_FINE 8u
static uint32_t emu_mode = EMU_FAST_ROM | EMU_FRAME_SKIP | EMU_WAIT_SKIP | EMU_SND_FINE;   /* all on at start */
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
    uint32_t sram_n;                          /* of them the cartridge's: its program's save (lsdj_prog_save) */
    uint8_t (*fb)[LSDJ_W];                    /* LSDJ_H rows of shades */
    uint8_t *dirty;                           /* LSDJ_H flags: set when a row's pixels change (0: not kept) */
    uint32_t errors, last_error, last_addr;
    uint32_t skipped;                         /* instructions of LSDj's wait loop not run (emu_wait) */
    int16_t snd[AUDIO_SAMPLES_TOTAL];         /* the last frame's audio, interleaved stereo */
} emu LSDJ_POOL;

/* ---- the sound, piece by piece ----
 * LSDj's sound engine runs on the timer interrupt, about 6 times in a Game Boy frame, and writes the sound
 * registers then. minigb_apu makes a frame's sound in one go, so every write of a frame was heard from the frame's
 * start: a note began up to 16.7 ms early (by how much changes from note to note: an uneven beat), and of several
 * writes to one register in a frame (LSDj's kits, tables, fast envelopes) only the last was heard.
 * With EMU_SND_FINE, before a register is written (or read) the sound is made up to where in its frame the Game Boy
 * is; emu_frame makes the rest. */
static uint32_t emu_snd_n;                    /* stereo frames of this Game Boy frame's sound already made */
#define EMU_SND_Q ((AUDIO_SAMPLES << 20) / 70224u)   /* stereo frames per clock cycle, Q20 (a frame: 70224 cycles) */

static void emu_snd_to(uint32_t n)
{
    if (n > AUDIO_SAMPLES)
        n = AUDIO_SAMPLES;
    if (n > emu_snd_n) {
        minigb_apu_audio_render(&emu_apu, emu.snd + 2u * emu_snd_n, n - emu_snd_n);
        emu_snd_n = n;
    }
}

/* where in its frame the Game Boy is, in stereo frames of sound. A frame (emu_frame) runs from the start of one
 * VBLANK (line 144) to the next; with the screen off, Peanut-GB counts the frame in lcd_off_count */
static uint32_t emu_snd_now(void)
{
    const struct gb_s *gb = &emu.gb;
    uint32_t c, ly;
    if (gb->hram_io[IO_LCDC] & LCDC_ENABLE) {
        ly = gb->hram_io[IO_LY];
        c = (ly >= LCD_HEIGHT ? ly - LCD_HEIGHT : ly + LCD_VERT_LINES - LCD_HEIGHT) * LCD_LINE_CYCLES +
            (uint32_t)gb->counter.lcd_count;
    } else {
        c = (uint32_t)gb->counter.lcd_off_count;
    }
    return (c * EMU_SND_Q) >> 20;             /* (c <= 71000: no overflow) */
}

uint8_t audio_read(const uint16_t a)
{
    if (emu_mode & EMU_SND_FINE)
        emu_snd_to(emu_snd_now());            /* (NR52 tells which channels still sound) */
    return minigb_apu_audio_read(&emu_apu, a);
}

void audio_write(const uint16_t a, const uint8_t v)
{
    if (emu_mode & EMU_SND_FINE)
        emu_snd_to(emu_snd_now());
    minigb_apu_audio_write(&emu_apu, a, v);
}

/* ---- LSDj's wait loop ----
 * Between its interrupts LSDj's main loop does nothing but read two flags in work RAM, over and over:
 *     P:  XOR A / LD HL,f1 / OR (HL) / JP NZ,.. / XOR A / LD HL,f2 / OR (HL) / JP Z,P
 * 8 instructions, 76 clock cycles a turn. Measured on the host with LSDj 9.4.2 (2026-10-13): 27 .. 31 % of all the
 * instructions it runs while a full song plays, 55 % with the demo (with nothing playing it sleeps in HALT, which
 * Peanut-GB skips by itself). Only an interrupt
 * handler can end the loop, and an interrupt needs one of Peanut-GB's timing events first (a timer overflow, a
 * screen line or mode, a serial byte). So while the loop turns and no event is due, emu_wait moves the clocks on
 * by as many of the loop's instructions as fit before the next event, without running them: the registers and the
 * counters end exactly as if they had run (tests/lsdj/lsdj_emu_test.c: the whole machine is the same after every
 * frame). The instruction that meets the event is run as ever.
 * The loop is found in the ROM by its instructions (emu_init); another ROM without it is run as before. */
static const uint8_t EMU_WAIT_AT[8] = {0, 1, 4, 5, 8, 9, 12, 13};      /* the 8 instructions: offset from P, */
static const uint8_t EMU_WAIT_CYC[8] = {4, 12, 8, 12, 4, 12, 8, 16};   /* clock cycles (JP NZ not taken, JP Z taken) */
#define EMU_WAIT_TURN 76u
#define EMU_WAIT_NONE (1u << 16)              /* no loop found: out of the program counter's reach */
static uint32_t emu_wait_pc = EMU_WAIT_NONE;  /* P */
static uint16_t emu_wait_f[2];                /* f1, f2 (work RAM, 0xC000 .. 0xDFFF) */

static void emu_wait_find(const lsdj_bank_t *rom, uint32_t nbanks)
{
    uint32_t p, f1, f2;
    emu_wait_pc = EMU_WAIT_NONE;
    for (p = 0x150u; p + 16u <= LSDJ_BANK; p++) {
#define B(i) lsdj_rom_byte(rom, nbanks, p + (i))
        if (B(0) != 0xAF || B(1) != 0x21 || B(4) != 0xB6 || B(5) != 0xC2 || B(8) != 0xAF || B(9) != 0x21 ||
            B(12) != 0xB6 || B(13) != 0xCA || (B(14) | (uint32_t)B(15) << 8) != p)
            continue;
        f1 = B(2) | (uint32_t)B(3) << 8;
        f2 = B(10) | (uint32_t)B(11) << 8;
#undef B
        if (f1 - WRAM_0_ADDR >= WRAM_SIZE || f2 - WRAM_0_ADDR >= WRAM_SIZE)
            continue;
        emu_wait_pc = p;
        emu_wait_f[0] = (uint16_t)f1;
        emu_wait_f[1] = (uint16_t)f2;
        return;
    }
}

/* the Game Boy is at P + at (0 .. 13). 1 = some of the loop's instructions were skipped: the clocks moved on, the
 * program counter and HL are where running them would have left them; 0 = nothing done: run the instruction */
static int emu_wait(struct gb_s *gb)
{
    static const uint8_t TAC_SHIFT[4] = {10, 4, 6, 8};       /* a timer count: 1024, 16, 64, 256 cycles */
    uint32_t at = gb->cpu_reg.pc.reg - emu_wait_pc, i, room, c, v, n, sh = 0;
    if (!(emu_mode & EMU_WAIT_SKIP))
        return 0;
    for (i = 0; EMU_WAIT_AT[i] != at;)
        if (++i == 8u)
            return 0;                         /* (inside an instruction: not the loop) */
    /* the loop is turning: both flags clear, and the registers as its own instructions leave them (A = 0, flags
     * Z only, HL = the flag last loaded): so skipping instructions changes nothing but HL and the clocks */
    if (gb->cpu_reg.a || !gb->cpu_reg.f.f_bits.z || gb->cpu_reg.f.f_bits.n || gb->cpu_reg.f.f_bits.h ||
        gb->cpu_reg.f.f_bits.c || gb->cpu_reg.hl.reg != emu_wait_f[i - 2u < 4u ? 0 : 1] ||
        (gb->wram[emu_wait_f[0] - WRAM_0_ADDR] | gb->wram[emu_wait_f[1] - WRAM_0_ADDR]))
        return 0;
    if (gb->gb_halt || (gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & ANY_INTR) || gb->mbc == 3)
        return 0;                             /* an interrupt is waiting (or a clock this code does not count) */
    /* room: the clock cycles that may pass with the timing code (the end of peanut_gb.h's __gb_step_cpu) only
     * counting. The screen: the next line or mode */
    if (gb->hram_io[IO_LCDC] & LCDC_ENABLE) {
        v = gb->hram_io[IO_STAT] & STAT_MODE;
        c = v == IO_STAT_MODE_OAM_SCAN ? LCD_MODE2_OAM_SCAN_END :
            v == IO_STAT_MODE_LCD_DRAW ? LCD_MODE3_LCD_DRAW_END : LCD_LINE_CYCLES;
        if (gb->counter.lcd_count >= c)
            return 0;
        room = c - 1u - (uint32_t)gb->counter.lcd_count;
    } else {
        if (gb->counter.lcd_off_count >= LCD_FRAME_CYCLES)
            return 0;
        room = LCD_FRAME_CYCLES - 1u - (uint32_t)gb->counter.lcd_off_count;
    }
    if (gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK) {          /* the timer: its overflow */
        sh = TAC_SHIFT[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK];
        c = (0x100u - gb->hram_io[IO_TIMA]) << sh;
        if (gb->counter.tima_count >= c)
            return 0;
        c -= 1u + (uint32_t)gb->counter.tima_count;
        room = c < room ? c : room;
    }
    if (gb->hram_io[IO_SC] & SERIAL_SC_TX_START) {           /* a serial transfer: its end */
        if (gb->gb_serial_tx || gb->counter.serial_count >= SERIAL_CYCLES)
            return 0;
        c = SERIAL_CYCLES - 1u - (uint32_t)gb->counter.serial_count;
        room = c < room ? c : room;
    }
    /* whole turns, then single instructions, while they fit */
    n = room < 8u * EMU_WAIT_TURN ? 0 : room / EMU_WAIT_TURN;
    for (c = n * EMU_WAIT_TURN; room - c >= EMU_WAIT_TURN; c += EMU_WAIT_TURN)
        n++;
    for (n *= 8u; c + EMU_WAIT_CYC[i] <= room; i = (i + 1u) & 7u, n++)
        c += EMU_WAIT_CYC[i];
    if (!c)
        return 0;
    v = (uint32_t)gb->counter.div_count + c;                 /* the clocks, as c cycles of instructions move them */
    gb->hram_io[IO_DIV] = (uint8_t)(gb->hram_io[IO_DIV] + v / DIV_CYCLES);
    gb->counter.div_count = v % DIV_CYCLES;
    if (gb->hram_io[IO_SC] & SERIAL_SC_TX_START)
        gb->counter.serial_count += c;
    if (gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK) {
        v = (uint32_t)gb->counter.tima_count + c;
        gb->hram_io[IO_TIMA] = (uint8_t)(gb->hram_io[IO_TIMA] + (v >> sh));
        gb->counter.tima_count = v & ((1u << sh) - 1u);
    }
    if (gb->hram_io[IO_LCDC] & LCDC_ENABLE)
        gb->counter.lcd_count += c;
    else
        gb->counter.lcd_off_count += c;
    gb->cpu_reg.pc.reg = (uint16_t)(emu_wait_pc + EMU_WAIT_AT[i]);
    gb->cpu_reg.hl.reg = emu_wait_f[i - 2u < 4u ? 0 : 1];
    emu.skipped += n;
    return 1;
}

static uint8_t emu_rom_read(struct gb_s *gb, const uint_fast32_t a)
{
    (void)gb;
    return lsdj_rom_byte(emu_rom_t, emu_rom_n, (uint32_t)a);
}
static uint8_t emu_ram_read(struct gb_s *gb, const uint_fast32_t a)
{
    (void)gb;
    return a < emu.sram_n ? emu.sram[a] : 0xFFu;
}
static void emu_ram_write(struct gb_s *gb, const uint_fast32_t a, const uint8_t v)
{
    (void)gb;
    if (a < emu.sram_n)
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

/* 0 = running; else Peanut-GB's gb_init error (bad header checksum, unsupported cartridge) or -1 (neither LSDj's
 * cartridge nor DMGo's: lsdj_prog) */
static int emu_init(const lsdj_bank_t *rom, uint32_t nbanks, uint8_t *sram, uint8_t (*fb)[LSDJ_W])
{
    enum gb_init_error_e r;
    uint8_t h[LSDJ_HDR];
    emu.rom = emu_rom_t = rom;
    emu.nbanks = emu_rom_n = nbanks;
    emu_lo_p = nbanks ? rom[0].p : 0;
    emu_lo_n = nbanks ? rom[0].n : 0;
    emu_hi_set(~0u);                          /* no bank cached yet */
    emu.sram = sram;
    emu.fb = fb;
    emu.dirty = 0;
    emu.errors = 0;
    emu.skipped = 0;
    emu_snd_n = AUDIO_SAMPLES;                /* (gb_init writes a sound register: no sound made for it) */
    emu_wait_find(rom, nbanks);
    lsdj_header(rom, nbanks, h);
    emu.sram_n = lsdj_prog_save(lsdj_prog(h));
    if (!emu.sram_n)
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
    struct gb_s *gb = &emu.gb;
    gb->direct.joypad = joypad;
    gb->direct.frame_skip = (emu_mode & EMU_FRAME_SKIP) != 0;
    emu_snd_n = 0;
    gb->gb_frame = false;                     /* (Peanut-GB's gb_run_frame, but for the wait loop) */
    while (!gb->gb_frame)
        if ((uint32_t)gb->cpu_reg.pc.reg - emu_wait_pc > 13u || !emu_wait(gb))
            __gb_step_cpu(gb);
    emu_snd_to(AUDIO_SAMPLES);                /* the rest of the frame's sound (all of it, without EMU_SND_FINE) */
}
