/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* Host test of the LSDj unit (firmware/src/lsdj/lsdj.c) as the core drives it: its hooks called like the main loop
 * and the audio ISR do, the core API stubbed with a 240x240 screen in RAM.
 *   lsdj_unit_test none      no cartridge block in the firmware: PLAY says so, nothing runs
 *   lsdj_unit_test damaged   a block with one wrong byte: the same
 *   lsdj_unit_test own       a block of a small Game Boy program of our own (2 banks; no LSDj data): PLAY runs it
 *   lsdj_unit_test           the rest, with the block made from the owner's ROM (build/lsdj/cart.bin: a build with
 *                            LSDJ_ROM; skipped without it). Writes build/lsdj/unit_screen.ppm.
 *   cc -O1 -w -Ifirmware/src -Ifirmware/src/lsdj -o build/tests/lsdj_unit_test tests/lsdj/lsdj_unit_test.c */
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
static uint16_t last_copy[240 * 240];
static uint32_t last_n, last_h;
static int lcd_idle = 1;
static char keys_text[40];                    /* the right part of the status line, as last drawn */
static char big_text[40], small_text[48];     /* the last text in each font drawn in the middle of the screen */
/* the app's flash slot after the firmware: empty (0xFF), or with a cartridge block at its end */
#define SLOT_ROOM (512u * 1024u)
static uint8_t slot_tail[SLOT_ROOM];

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
    last_h = (uint32_t)h;
    memcpy(last_copy, px, last_n * 2u);
    last_px_busy = 1;
    blits++;
}
void core_fill(int x, int y, int w, int h, int colour) { (void)x; (void)y; (void)w; (void)h; (void)colour; dma_check(); }
void core_text(int x, int y, int w, int font, const char *s, int fg, int bg, int c)
{
    (void)w; (void)fg; (void)bg; (void)c;
    dma_check();
    texts++;
    if (x == 112 && strlen(s) < sizeof keys_text)
        strcpy(keys_text, s);
    if (x == 0 && y >= 90 && y < 140 && strlen(s) < sizeof big_text)
        strcpy(font == CORE_FONT_M ? big_text : small_text, s);
}
const uint8_t *core_cart_end(void) { return slot_tail + SLOT_ROOM; }
uint32_t core_cart_room(void) { return SLOT_ROOM; }
static int32_t pot = 1023;
int core_lcd_idle(void) { return lcd_idle; }
int32_t core_master_pot(void) { return pot; }
uint32_t core_buttons(void) { return buttons; }
uint32_t core_notes(void) { return notes; }
uint32_t core_button_bit(int label) { return 1u << label; }   /* (the stub: matrix id = label) */
uint32_t core_play_bit(void) { return 1u << 10; }
uint32_t core_rec_bit(void) { return 1u << 11; }
static char con_log[4096];                    /* what the unit printed on the console */
static char con_line[64];                     /* the console line being carried out */
void core_con_puts(const char *s)
{
    fputs(s, stdout);
    if (strlen(con_log) + strlen(s) < sizeof con_log)
        strcat(con_log, s);
}
const char *core_con_line(void) { return con_line; }
static int32_t enc_algo, enc_preset;          /* knob steps waiting, as the panel keeps them */
int32_t core_enc_steps(int knob)
{
    int32_t *e = knob == CORE_ENC_PRESET ? &enc_preset : &enc_algo, v = *e;
    *e = 0;
    return v;
}

/* the store's flash (0xA0000..0xDBFFF) as a NOR flash: bits only go to 0, a sector erase sets 4 KiB to 0xFF */
static uint8_t flash_sim[60u * 4096u];
static uint32_t flash_erases, flash_progs, flash_bad;
const uint8_t *core_flash_ptr(uint32_t off)
{
    return off >= 0xA0000u && off < 0xDC000u ? flash_sim + (off - 0xA0000u) : 0;
}
int core_flash_erase(uint32_t off)
{
    if (off < 0xA0000u || off > 0xDC000u - 4096u || (off & 0xFFFu)) {
        flash_bad++;
        return -8;
    }
    memset(flash_sim + (off - 0xA0000u), 0xFF, 4096);
    flash_erases++;
    return 0;
}
int core_flash_prog(uint32_t off, const void *p, uint32_t n)
{
    uint32_t i;
    if (off < 0xA0000u || off > 0xDC000u || n > 0xDC000u - off) {
        flash_bad++;
        return -8;
    }
    for (i = 0; i < n; i++)
        flash_sim[off - 0xA0000u + i] &= ((const uint8_t *)p)[i];
    flash_progs++;
    return 0;
}

#define LSDJ_CART_MAKE 1                      /* (the damaged block is made here) */
#include "lsdj/lsdj.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void pass_ms(uint32_t t)               /* one main-loop pass, then t ms of the audio ISR */
{
    static int32_t out[2 * 32];
    uint32_t i;
    ble_service();
    ble_draw();
    for (i = 0; i < t * 441u / 10u; i += 32u) /* at 44.1 kHz in blocks of 32 (as CTL) */
        lsdj_audio(out, 32);
    ms += t;
}
static void pass(void) { pass_ms(20); }       /* (the core's pass is 15 ms or more; a Game Boy frame is 16.7 ms) */

static void console(const char *line)         /* as the core does: the line `ble ...`, then the unit's ble_status */
{
    strcpy(con_line, line);
    con_log[0] = 0;
    ble_status();
    con_line[0] = 0;
}

/* times `key` goes down (with `with` down). Passes of 15 ms with the ring filled: no more than one Game Boy frame
 * a pass, so st.pad after a pass is that frame's keys */
static uint32_t pad_presses(uint8_t key, uint8_t with, uint32_t passes)
{
    uint32_t n = 0, was = 0, i, f;
    for (i = 0; i < passes; i++) {
        f = st.frames;
        pass_ms(15);
        CHECK(st.frames - f <= 1u);
        if (st.frames != f) {                         /* a Game Boy frame ran: its keys */
            uint32_t now = !(st.pad & key);
            if (now && !was) {
                n++;
                CHECK(!(st.pad & with));
            }
            was = now;
        }
    }
    return n;
}

static void test_knobs_and_shortcuts(void)    /* the knobs are the cursor; PRESETS and OCT- / OCT+ change LSDj's screen */
{
    uint32_t i;
    for (i = 0; i < 200; i++)                         /* (the ring filled: see pad_presses) */
        pass_ms(15);
    CHECK(st.pad == 0xFF);
    ble_key(0, 3);                                    /* SELECT knob, 3 steps clockwise */
    CHECK(pad_presses(JOYPAD_RIGHT, 0, 40) == 3);
    ble_key(0, -2);
    CHECK(pad_presses(JOYPAD_LEFT, 0, 40) == 2);
    enc_algo = 2;                                     /* ALGORITHM clockwise: down */
    ble_key(0, 0);
    CHECK(enc_algo == 0 && pad_presses(JOYPAD_DOWN, 0, 40) == 2);
    enc_algo = -1;
    ble_key(0, 0);
    CHECK(pad_presses(JOYPAD_UP, 0, 40) == 1);
    notes = 1u << 24;                                 /* the top F held: A + the knob */
    ble_key(0, 2);
    CHECK(pad_presses(JOYPAD_RIGHT, JOYPAD_A, 40) == 2);
    notes = 0;
    enc_preset = 1;                                   /* PRESETS: SELECT + RIGHT */
    ble_key(0, 0);
    CHECK(pad_presses(JOYPAD_RIGHT, JOYPAD_SELECT, 40) == 1);
    enc_preset = -1;
    ble_key(0, 0);
    CHECK(pad_presses(JOYPAD_LEFT, JOYPAD_SELECT, 40) == 1);
    ble_key(core_button_bit(CORE_BTN_OCTUP), 0);      /* OCT+: SELECT + UP */
    CHECK(pad_presses(JOYPAD_UP, JOYPAD_SELECT, 40) == 1);
    ble_key(core_button_bit(CORE_BTN_OCTDN), 0);      /* OCT-: SELECT + DOWN */
    CHECK(pad_presses(JOYPAD_DOWN, JOYPAD_SELECT, 40) == 1);
    CHECK(st.pad == 0xFF && emu.errors == 0);
    console("ble hold");                              /* held for the host: turns are dropped, not kept for later */
    enc_algo = 3;
    enc_preset = 2;
    ble_key(0, 4);
    console("ble go");
    ble_key(0, 0);
    CHECK(enc_algo == 0 && enc_preset == 0);
    for (i = 0; i < 12; i++) {                        /* (the ring is low after the hold: any frame's keys) */
        pass();
        CHECK(st.pad == 0xFF);
    }
}

static void test_flash(void)                  /* REC held 1 s saves; PLAY after power-on loads it; a kit bank */
{
    static const uint8_t mark[4] = {0x12, 0x34, 0x56, 0x78}, kit[3] = {0x60, 0x40, 0xA1};
    char line[64];
    uint32_t i, f, c;
    CHECK(!store.have && flash_erases == 0 && flash_progs == 0);     /* starting and running wrote nothing */
    console("ble info");
    CHECK(strstr(con_log, " kits 00 0C cap 1CF00 used 00000 saved 0") != 0);
    console("ble hold");
    sprintf(line, "ble w 1FF10 12345678 %04X", lsdj_crc16(mark, 4));
    console(line);
    console("ble go");
    buttons = core_rec_bit();                         /* REC held: not at once */
    for (i = 0; i < 40; i++)                          /* 0.8 s */
        pass();
    CHECK(!store.have && flash_progs == 0);
    for (i = 0; i < 20; i++)                          /* past 1 s: saved */
        pass();
    CHECK(store.have && store.seq == 1 && flash_progs > 0 && !strncmp(st.note, "SAVED ", 6));
    f = flash_erases;
    for (i = 0; i < 110; i++)                         /* still held: once only; the note goes after 2 s */
        pass();
    CHECK(store.seq == 1 && flash_erases == f && st.note[0] == 0);
    buttons = 0;
    pass();
    console("ble info");
    CHECK(strstr(con_log, " saved 1") && !strstr(con_log, " used 00000"));
    console("ble save");                              /* the console only queues; the next step writes */
    CHECK(!strcmp(con_log, "ok\r\n") && store.seq == 1);
    console("ble st");
    CHECK(!strcmp(con_log, "ok busy\r\n"));
    pass();
    console("ble st");
    CHECK(!strcmp(con_log, "ok 0\r\n") && store.seq == 2 && store.newest == 1);
    memset(sram, 0xEE, LSDJ_SRAM);                    /* as after power-off and on: PLAY loads the save */
    lsdj_start();
    CHECK(st.error == 0 && !memcmp(sram + 0x1FF10, mark, 4) && !memcmp(sram + 0x1FF00, "\xDE\xAD\xBE\xEF", 4));
    for (i = 0; i < 100; i++)
        pass();
    CHECK(emu.errors == 0 && st.frames > 50);
    console("ble hold");                              /* a kit bank: one more bank, the kit into it */
    console("ble kits 1");
    pass();                                           /* (held: the step still carries the request out) */
    console("ble st");
    CHECK(!strcmp(con_log, "ok 0\r\n") && store.k == 1 && store.have);
    console("ble kb 0");
    CHECK(!strcmp(con_log, "ok 22\r\n"));             /* the first free bank of LSDj 9.4.2: 34 */
    console("ble b 22 0 2");
    CHECK(!strncmp(con_log, "ok FFFF ", 8));          /* free */
    console("ble k 0");
    sprintf(line, "ble kd 0 6040A1 %04X", lsdj_crc16(kit, 3));
    console(line);
    CHECK(!strcmp(con_log, "ok\r\n"));
    for (i = 0, c = 0xFFFFFFFFu; i < LSDJ_BANK; i++)
        c = lsdj_crc32_add(c, kit_buf[i]);
    sprintf(line, "ble kc %08X", (unsigned)~c);
    console(line);
    pass();
    console("ble st");
    CHECK(!strcmp(con_log, "ok 0\r\n"));
    console("ble b 22 0 3");
    CHECK(!strncmp(con_log, "ok 6040A1 ", 10));       /* the Game Boy's bank 34 is the flash kit bank */
    console("ble go reset");
    for (i = 0; i < 100; i++)
        pass();
    CHECK(emu.errors == 0);
    lsdj_start();                                     /* power-on again: K and the kit are there */
    CHECK(store.k == 1 && lsdj_rom_byte(rom_tab, LSDJ_ROM_BANKS, 34u * LSDJ_BANK + 2u) == 0xA1 && !memcmp(sram + 0x1FF10, mark, 4));
    console("ble hold");
    console("ble kx 0");
    pass();
    console("ble b 22 0 2");
    CHECK(!strncmp(con_log, "ok FFFF ", 8));          /* erased: free again */
    console("ble go");
    CHECK(flash_bad == 0);
}

/* A pass of the core lasts 15 ms or more and a Game Boy frame is 16.7 ms of sound: one frame a pass makes little
 * more sound than is played, and nothing after a slow pass. So when the ring is under half, the step runs a second
 * frame in the same pass. GAPS on the status line: the seconds in which the sound ran dry all the same (the FM-1
 * too slow), but not those the host held the Game Boy in. */
static void test_gaps(void)
{
    uint32_t i, g, f, d;
    char b[24];
    for (i = 0; i < 100; i++)                 /* passes of 20 ms: 882 samples played, a frame makes 738 */
        pass();
    g = st.gaps;
    f = st.frames;
    for (i = 0; i < 150; i++)                 /* 3 s: a second frame now and then keeps up */
        pass();
    CHECK(st.gaps == g);
    CHECK(st.frames >= f + 180u && st.frames <= f + 184u);       /* (150 passes play 150 x 896 samples: 182 frames) */
    CHECK(lsdj_ring_fill(&ring) >= 700u);                        /* and the ring not near empty */
    for (i = 0; i < 100; i++)                 /* passes of 15 ms: one frame each is enough, the ring fills */
        pass_ms(15);
    f = st.frames;
    d = st.doubles;
    for (i = 0; i < 200; i++)
        pass_ms(15);
    CHECK(st.gaps == g && lsdj_ring_fill(&ring) > RING_N / 2u);
    CHECK(st.doubles == d);                                      /* ... and no second frame is run */
    CHECK(st.frames >= f + 180u && st.frames <= f + 184u);       /* (200 passes play 200 x 672 samples: 182 frames) */
    for (i = 0; i < 75; i++)                  /* 3 s of passes of 40 ms: two frames a pass are not enough */
        pass_ms(40);
    CHECK(st.gaps >= g + 2u && st.gaps <= g + 3u);
    for (i = 0; i < 60; i++)
        pass();
    sprintf(b, "GAPS %u", (unsigned)st.gaps);
    CHECK(!strcmp(keys_text, b));
    console("ble hold");
    g = st.gaps;
    for (i = 0; i < 100; i++)                 /* held 2 s: silence, and no gap of the FM-1's */
        pass();
    console("ble go");
    for (i = 0; i < 100; i++)
        pass();
    CHECK(st.gaps == g);
    console("ble");
    CHECK(strstr(con_log, "gap seconds ") != 0 && strstr(con_log, "wait skipped ") != 0);
    CHECK(strstr(con_log, "sec_frame_max_us ") != 0 && strstr(con_log, "sec_pass_max_us ") != 0);
}

static void test_bench(void)                  /* `ble key`, `ble mode`: the keys and options from the console */
{
    uint32_t i, f, down = 0;
    for (i = 0; i < 200; i++)                 /* (the ring filled: one frame a pass) */
        pass_ms(15);
    CHECK(st.pad == 0xFF);
    console("ble key 14 3");                  /* SELECT + RIGHT for 3 frames */
    CHECK(!strcmp(con_log, "ok\r\n"));
    for (i = 0, f = st.frames; i < 12; i++) {
        uint32_t f0 = st.frames;
        pass_ms(15);
        if (st.frames != f0 && st.pad != 0xFF) {
            CHECK(st.pad == (uint8_t)~(JOYPAD_SELECT | JOYPAD_RIGHT));
            down++;
        }
    }
    CHECK(down == 3 && st.pad == 0xFF && st.frames > f + 6);
    console("ble mode");
    CHECK(!strcmp(con_log, "ok F\r\n"));
    console("ble mode 7");
    CHECK(!strcmp(con_log, "ok 7\r\n") && emu_mode == 7);
    pass_ms(15);
    CHECK(strstr(keys_text, " OLD") != 0);    /* ... and the status line says so */
    console("ble mode F");
    CHECK(emu_mode == 15);
}

static void test_console(void)                /* after PLAY: `ble ...` holds and restarts the Game Boy */
{
    static const uint8_t d[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    char line[64];
    uint32_t i, f, t;
    console("ble");
    CHECK(strstr(con_log, "lsdj running") && !strstr(con_log, "ok"));   /* bare: the status, as before */
    console("  ble  ");
    CHECK(strstr(con_log, "lsdj running") != 0);
    console("ble info");
    CHECK(!strcmp(con_log, "ok v2 started 1 held 0 banks 40 kits 00 0C cap 1CF00 used 00000 saved 0\r\n"));
    console("ble r 0 4");
    CHECK(!strcmp(con_log, "err hold\r\n"));          /* not while the Game Boy runs */
    console("ble hold");
    t = texts;
    f = st.frames;
    for (i = 0; i < 20; i++)
        pass();
    CHECK(st.frames == f);                            /* held: no Game Boy frame */
    CHECK(texts > t);                                 /* "USB" on the status line */
    sprintf(line, "ble w 1FF00 DEADBEEF %04X", lsdj_crc16(d, 4));   /* a free block at the end of the save */
    console(line);
    CHECK(!strcmp(con_log, "ok\r\n") && !memcmp(sram + 0x1FF00, d, 4));
    console("ble go reset");
    for (i = 0; i < 100; i++)
        pass();
    CHECK(xfer.reset_req == 0 && st.frames > f && emu.errors == 0);   /* LSDj started again and runs */
    CHECK(!memcmp(sram + 0x1FF00, d, 4));             /* the save RAM as it was written */
    console("ble hold");
    f = st.frames;
    for (i = 0; i < 240; i++)                         /* 4.8 s without a command */
        pass();
    CHECK(xfer.held == 1 && st.frames == f);
    for (i = 0; i < 20; i++)                          /* past 5 s: the host went away */
        pass();
    CHECK(xfer.held == 0 && st.frames > f);
}

/* PLAY with no cartridge block, or a damaged one: a message, and nothing runs, whatever is pressed after */
static int test_no_rom(int damaged)
{
    static uint8_t rom[2u * LSDJ_BANK], save[LSDJ_CART_SAVE];
    uint32_t i;
    memset(flash_sim, 0xFF, sizeof flash_sim);
    memset(slot_tail, 0xFF, sizeof slot_tail);
    if (damaged) {                            /* a whole block of a made-up ROM, then one bit of it wrong */
        uint8_t x = 0;
        int32_t n;
        for (i = 0; i < sizeof rom; i++)
            rom[i] = (uint8_t)(i * 7u);
        rom[0x147] = 0x1B;
        rom[0x149] = 0x04;
        for (i = 0x134; i <= 0x14C; i++)
            x = (uint8_t)(x - rom[i] - 1u);
        rom[0x14D] = x;
        n = lsdj_cart_make(slot_tail, sizeof slot_tail, rom, sizeof rom, save);
        CHECK(n > 0);
        memmove(slot_tail + SLOT_ROOM - (uint32_t)n, slot_tail, (uint32_t)n);
        memset(slot_tail, 0xFF, SLOT_ROOM - (uint32_t)n);
        slot_tail[SLOT_ROOM - 4000u] ^= 0x10;
    }
    ble_draw();
    for (i = 0; i < 10; i++)
        pass();
    console("ble info");
    CHECK(!strncmp(con_log, "ok v2 started 0 held 0 banks 00 ", 32));
    ble_key(core_play_bit(), 0);              /* PLAY */
    CHECK(!ble_started() && st.error == (damaged ? -4 : -3));
    CHECK(!strcmp(big_text, damaged ? "LSDJ ROM DAMAGED" : "NO LSDJ ROM"));
    CHECK(!strcmp(small_text, damaged ? "INSTALL IT AGAIN" : "PUT YOURS IN WITH THE INSTALL PAGE"));
    for (i = 0; i < 50; i++) {
        ble_key(i == 20 ? core_play_bit() : 0, 1);     /* PLAY again, a knob: still nothing */
        pass();
    }
    CHECK(!ble_started() && st.frames == 0 && flash_erases == 0 && flash_progs == 0);
    console("ble hold");
    CHECK(!strcmp(con_log, "err not started\r\n"));
    console("ble b 0 0 4");
    CHECK(!strcmp(con_log, "err range\r\n"));         /* no bank to read */
    console("ble");
    CHECK(strstr(con_log, damaged ? "lsdj stopped error -4" : "lsdj stopped error -3") != 0);
    printf(fails ? "lsdj_unit_test (%s): %d FAILED\n" : "lsdj_unit_test (%s): all passed\n",
           damaged ? "a damaged block" : "no block", fails);
    return fails != 0;
}

/* PLAY with a block of our own program (as tests/lsdj/lsdj_install_test.py's test_rom: a timer interrupt, a note every
 * 64th, a wait loop like LSDj's, one dark row in every tile): it starts and runs, a ROM of 2 banks */
static int test_own_rom(void)
{
    static uint8_t rom[2u * LSDJ_BANK], save[LSDJ_CART_SAVE];
    static const struct { uint16_t at; uint8_t n; uint8_t b[60]; } P[] = {
        {0x040, 8, {0xF5, 0x3E, 0x01, 0xEA, 0x01, 0xC1, 0xF1, 0xD9}},
        {0x050, 8, {0xF5, 0x3E, 0x01, 0xEA, 0x00, 0xC1, 0xF1, 0xD9}},
        {0x100, 4, {0x00, 0xC3, 0x50, 0x01}},
        {0x150, 59, {0xF3, 0x31, 0xFF, 0xDF, 0x3E, 0x80, 0xE0, 0x26, 0x3E, 0x77, 0xE0, 0x24, 0x3E, 0xFF, 0xE0, 0x25,
                     0x3E, 0xB0, 0xE0, 0x11, 0x3E, 0xF0, 0xE0, 0x12, 0x3E, 0x83, 0xE0, 0x13,
                     0x3E, 0x49, 0xE0, 0x06, 0xE0, 0x05, 0x3E, 0x06, 0xE0, 0x07,
                     0xAF, 0xE0, 0x0F, 0xEA, 0x00, 0xC1, 0xEA, 0x01, 0xC1, 0xEA, 0x02, 0xC1,
                     0x21, 0x00, 0x80, 0x3E, 0xFF, 0x22, 0x77, 0x3E, 0x05}},
        {0x18B, 6, {0xE0, 0xFF, 0xFB, 0xC3, 0x00, 0x02}},
        {0x200, 16, {0xAF, 0x21, 0x00, 0xC1, 0xB6, 0xC2, 0x40, 0x02, 0xAF, 0x21, 0x01, 0xC1, 0xB6, 0xCA, 0x00, 0x02}},
        {0x210, 7, {0xAF, 0xEA, 0x01, 0xC1, 0xC3, 0x00, 0x02}},
        {0x240, 21, {0xAF, 0xEA, 0x00, 0xC1, 0x21, 0x02, 0xC1, 0x34, 0x7E, 0xE6, 0x3F, 0xC2, 0x00, 0x02,
                     0x3E, 0xC6, 0xE0, 0x14, 0xC3, 0x00, 0x02}}};
    uint32_t i;
    uint8_t x = 0;
    int32_t n;
    memset(rom, 0xFF, sizeof rom);
    for (i = 0; i < sizeof P / sizeof P[0]; i++)
        memcpy(rom + P[i].at, P[i].b, P[i].n);
    memset(rom + 0x104, 0, 0x4C);
    rom[0x147] = 0x1B;
    rom[0x148] = 0x00;
    rom[0x149] = 0x04;
    for (i = 0x134; i <= 0x14C; i++)
        x = (uint8_t)(x - rom[i] - 1u);
    rom[0x14D] = x;
    memset(flash_sim, 0xFF, sizeof flash_sim);
    memset(slot_tail, 0xFF, sizeof slot_tail);
    n = lsdj_cart_make(slot_tail, sizeof slot_tail, rom, sizeof rom, save);
    CHECK(n > 0);
    memmove(slot_tail + SLOT_ROOM - (uint32_t)n, slot_tail, (uint32_t)n);
    memset(slot_tail, 0xFF, SLOT_ROOM - (uint32_t)n);
    ble_draw();
    ble_key(core_play_bit(), 0);              /* PLAY */
    CHECK(ble_started() && st.error == 0 && cart.nbanks == 2 && nkit == 0);
    for (i = 0; i < 300; i++)                 /* 6 s */
        pass();
    CHECK(st.frames > 300 && emu.errors == 0 && st.gaps == 0);
    CHECK(emu.skipped > st.frames * 3000u);   /* (its wait loop is found and skipped) */
    {
        int32_t o[64], k, loud = 0;
        for (i = 0; i < 400 && !loud; i++) {  /* a note within 8 s */
            ble_service();
            lsdj_audio(o, 32);
            for (k = 0; k < 64; k++)
                loud |= o[k] != 0;
            ms += 20;
        }
        CHECK(loud);
    }
    console("ble info");
    CHECK(!strncmp(con_log, "ok v2 started 1 held 0 banks 02 kits 00 00 ", 43));
    console("ble b 1 0 2");
    CHECK(!strncmp(con_log, "ok FFFF ", 8));             /* its second bank: empty */
    console("ble b 2 0 2");
    CHECK(!strcmp(con_log, "err range\r\n"));           /* no third */
    printf(fails ? "lsdj_unit_test (our own program): %d FAILED\n" : "lsdj_unit_test (our own program): all passed\n", fails);
    return fails != 0;
}

int main(int argc, char **argv)
{
    uint32_t i, x, y, diff = 0;
    int32_t out[64];
    if (argc > 1)
        return !strcmp(argv[1], "own") ? test_own_rom() : test_no_rom(!strcmp(argv[1], "damaged"));
    {   /* the block made from the owner's ROM, at the end of the slot */
        FILE *f = fopen("build/lsdj/cart.bin", "rb");
        long n;
        if (!f) {
            printf("lsdj_unit_test: skipped (no build/lsdj/cart.bin: build with FELUCCA_LSDJ=1 LSDJ_ROM=your ROM)\n");
            return 0;
        }
        memset(slot_tail, 0xFF, sizeof slot_tail);
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        rewind(f);
        if (n <= 0 || n > (long)SLOT_ROOM || fread(slot_tail + SLOT_ROOM - (size_t)n, 1, (size_t)n, f) != (size_t)n) {
            printf("lsdj_unit_test: FAILED: build/lsdj/cart.bin\n");
            return 1;
        }
        fclose(f);
    }
    memset(flash_sim, 0xFF, sizeof flash_sim);   /* an empty store */
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
    for (i = 0; i < 150; i++)                 /* 3 s */
        pass();
    CHECK(st.gaps == 0 && !strcmp(keys_text, "GAPS 0"));   /* no gap: the empty ring at the start is not one */
    for (i = 0; i < 300; i++)                 /* 6 s */
        pass();
    CHECK(st.frames > 250 && emu.errors == 0);
    {   /* the MASTER pot: full -> the sound as made; down -> silence */
        int32_t a[64], k;
        int nz = 0;
        CHECK(st.master_q12 > 4000);
        lsdj_audio(a, 32);
        for (k = 0; k < 64; k++)
            nz |= a[k] != 0;
        pot = 0;
        for (k = 0; k < 60; k++)
            pass();
        CHECK(st.master_q12 == 0);
        lsdj_audio(a, 32);
        for (k = 0; k < 64; k++)
            CHECK(a[k] == 0);
        pot = 1023;
        for (k = 0; k < 60; k++)
            pass();
        (void)nz;
    }
    {   /* PLAY and SEQ held: START and SELECT reach the Game Boy, and the keys part of the status line is redrawn */
        uint32_t t = texts;
        buttons = core_play_bit() | core_button_bit(CORE_BTN_SEQ);
        pass();
        pass();
        CHECK(st.pad == (uint8_t)~(JOYPAD_START | JOYPAD_SELECT) && st.pad_shown == st.pad && texts > t);
        buttons = core_button_bit(CORE_BTN_SCL);                                     /* UP (button) + A (the top F) */
        notes = 1u << 24;
        pass();
        CHECK(st.pad == (uint8_t)~(JOYPAD_UP | JOYPAD_A));
        buttons = core_rec_bit() | core_button_bit(CORE_BTN_GLO) | core_button_bit(CORE_BTN_EDIT);   /* free now */
        notes = 1u << 26;                                                            /* B (the top G) */
        pass();
        CHECK(st.pad == (uint8_t)~JOYPAD_B);
        notes = 0;
        buttons = core_button_bit(CORE_BTN_OCTDN) | core_button_bit(CORE_BTN_OCTUP); /* the UBOOT pair: no key */
        pass();
        CHECK(st.pad == 0xFF);
        CHECK(emu_mode == (EMU_FAST_ROM | EMU_FRAME_SKIP | EMU_WAIT_SKIP | EMU_SND_FINE));   /* all on at start */
        buttons = 0;
        pass();
        CHECK(!strncmp(keys_text, "GAPS ", 5) && !strchr(keys_text + 5, ' '));       /* ... and then not named */
        ble_key(core_button_bit(CORE_BTN_FX), 0);                                    /* FX: frame skip off */
        CHECK(emu_mode == (EMU_FAST_ROM | EMU_WAIT_SKIP | EMU_SND_FINE));
        t = texts;
        pass();
        CHECK(texts > t && strstr(keys_text, " FULL") && !strstr(keys_text, "SLOW"));   /* the options redrawn */
        ble_key(core_button_bit(CORE_BTN_LFO), 0);                                   /* LFO: fast ROM read off */
        CHECK(emu_mode == (EMU_WAIT_SKIP | EMU_SND_FINE));
        ble_key(core_button_bit(CORE_BTN_EDIT), 0);                                  /* EDIT: the wait loop run */
        CHECK(emu_mode == EMU_SND_FINE);
        ble_key(core_button_bit(CORE_BTN_ENV), 0);                                   /* ENV: the sound per frame */
        CHECK(emu_mode == 0);
        pass();
        CHECK(strstr(keys_text, " SLOW FULL SPIN OLD") != 0);
        {
            uint32_t f = st.frames, k0 = emu.skipped;
            for (i = 0; i < 20; i++)
                pass();
            CHECK(st.frames >= f + 20 && emu.skipped == k0 && emu.errors == 0);     /* LSDj runs so too */
            ble_key(core_button_bit(CORE_BTN_EDIT) | core_button_bit(CORE_BTN_ENV), 0);
            for (i = 0; i < 20; i++)
                pass();
            CHECK(emu.skipped > k0 + 20u * 1500u && emu.errors == 0);               /* the wait loop skipped again */
        }
        ble_key(core_button_bit(CORE_BTN_FX) | core_button_bit(CORE_BTN_LFO), 0);
        CHECK(emu_mode == (EMU_FAST_ROM | EMU_FRAME_SKIP | EMU_WAIT_SKIP | EMU_SND_FINE));
        buttons = 0;
        pass();
        pass();
        CHECK(st.pad == 0xFF);
    }
    {   /* the push never waits: nothing while the panel is busy; else one transfer of up to 8 line pairs */
        uint32_t b = blits;
        memset(shown, 0xFF, sizeof shown);    /* everything changed */
        memset(dirty, 1, sizeof dirty);
        lcd_idle = 0;
        ble_draw();
        CHECK(blits == b);
        lcd_idle = 1;
        ble_draw();
        CHECK(blits == b + 1 && last_h == 24);
        ble_draw();
        CHECK(blits == b + 2);
    }
    for (i = 0; i < 200; i++) {               /* draw passes only: the screen catches up */
        ble_draw();
    }
    for (y = 0, x = 0; y < LSDJ_H; y++)       /* ... and no line is marked changed any more */
        x |= dirty[y];
    CHECK(x == 0);
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
    test_gaps();
    test_bench();
    test_console();
    test_knobs_and_shortcuts();
    test_flash();
    ble_status();
    printf(fails ? "lsdj_unit_test: %d FAILED\n" : "lsdj_unit_test: all passed (%u blits)\n",
           fails ? (unsigned)fails : blits);
    return fails != 0;
}
