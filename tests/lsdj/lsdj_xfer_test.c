/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* Host test of the LSDj unit's transfer commands (firmware/src/lsdj/lsdj_xfer.c). It also writes every command and
 * reply to build/tests/lsdj_xfer_vectors.txt (the words after `ble`): web/test_lsdj.mjs replays them against its
 * simulated device, so the page's tests and the firmware cannot drift apart.
 *   cc -Wall -Wno-unused-function -O1 -Ifirmware/src -o build/tests/lsdj_xfer_test tests/lsdj/lsdj_xfer_test.c
 * (lsdj_gb.c's screen and audio functions are not used here) */
#include <stdio.h>
#include <string.h>
#include "lsdj/lsdj_gb.c"
#include "lsdj/lsdj_xfer.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define NB 3u
static uint8_t sram[0x20000], bank[NB][LSDJ_BANK];
static lsdj_bank_t rom[NB];
static lsdj_xfer_t x;
static char out[LSDJ_XFER_OUT + 64];
static FILE *vec;
static uint32_t now;
static uint8_t kitbuf[LSDJ_BANK + 16];        /* (16 spare bytes: nothing may be written past the bank) */
static const uint8_t KIT_BANK[3] = {34, 35, 36};

static const char *say(const char *cmd)
{
    memset(out, 0x55, sizeof out);
    lsdj_xfer_cmd(&x, cmd, now, out);
    CHECK(strlen(out) < LSDJ_XFER_OUT);                /* the reply fits its buffer */
    CHECK((uint8_t)out[LSDJ_XFER_OUT] == 0x55);
    if (vec)
        fprintf(vec, "> %s\n< %s\n", cmd, out);
    return out;
}
static int is(const char *cmd, const char *reply) { return !strcmp(say(cmd), reply); }

static const char *data(const uint8_t *d, uint32_t n)  /* "ok HEX CRC", by the test's own printing */
{
    static char b[LSDJ_XFER_OUT + 8];
    char *o = b + sprintf(b, "ok ");
    uint32_t i;
    for (i = 0; i < n; i++)
        o += sprintf(o, "%02X", d[i]);
    sprintf(o, " %04X", lsdj_crc16(d, n));
    return b;
}

static uint32_t ref_crc32(const uint8_t *p, uint32_t n)   /* bit by bit: not the unit's table */
{
    uint32_t c = 0xFFFFFFFFu;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void setup(void)
{
    uint32_t i, b;
    for (i = 0; i < sizeof sram; i++)
        sram[i] = (uint8_t)(i * 7u + (i >> 8));
    for (b = 0; b < NB; b++) {
        memset(bank[b], 0xEE, LSDJ_BANK);              /* bytes past n must never be read */
        rom[b].p = bank[b];
        rom[b].n = (uint16_t)(100u * (b + 1u));
        for (i = 0; i < rom[b].n; i++)
            bank[b][i] = (uint8_t)(b * 31u + i);
    }
    memset(&x, 0, sizeof x);
    x.sram = sram;
    x.size = sizeof sram;
    x.rom = rom;
    x.nbanks = NB;
    x.kit = kitbuf;                           /* the flash store's side, as lsdj.c sets it */
    x.kit_bank = KIT_BANK;
    x.nkit = 3;
    x.cap = 0x1DFE0u;
    memset(kitbuf, 0x11, sizeof kitbuf);
}

static void flash_state(uint8_t kits, uint32_t cap, uint32_t used, uint8_t saved)   /* as lsdj.c after a request */
{
    x.kits = kits;
    x.cap = cap;
    x.used = used;
    x.saved = saved;
    if (vec)
        fprintf(vec, "! flash %X %X %X %X\n", (unsigned)kits, (unsigned)cap, (unsigned)used, (unsigned)saved);
}

static void req_done(uint8_t req, uint8_t arg, uint8_t res)    /* lsdj.c carried out the queued request */
{
    CHECK(x.req == req && x.req_arg == arg);
    x.req = LSDJ_REQ_NONE;
    x.res = res;
    if (vec)
        fprintf(vec, "! done %X\n", (unsigned)res);
}

static void test_crc(void)
{
    uint32_t c = 0xFFFFFFFFu, i;
    CHECK(lsdj_crc16((const uint8_t *)"123456789", 9) == 0x31C3u);      /* CRC-16/XMODEM's check value */
    for (i = 0; i < 9u; i++)
        c = lsdj_crc32_add(c, (uint8_t)"123456789"[i]);
    CHECK(~c == 0xCBF43926u);                                            /* CRC-32's check value */
}

static void test_state(void)
{
    CHECK(is("info", "ok v3 started 0 held 0 banks 03 kits 00 03 cap 1DFE0 used 00000 saved 0 prog 0 size 20000"));
    CHECK(is("hold", "err not started"));
    CHECK(is("r 0 4", "err hold"));
    CHECK(is("w 0 00 0000", "err hold"));
    CHECK(is("save", "err not started"));
    if (vec)
        fprintf(vec, "! started\n");
    x.started = 1;
    CHECK(is("  hold", "ok"));                          /* leading spaces: as the console passes the line */
    CHECK(is("info", "ok v3 started 1 held 1 banks 03 kits 00 03 cap 1DFE0 used 00000 saved 0 prog 0 size 20000"));
    CHECK(is("info x", "err ?"));
    x.prog = 1;                                         /* DMGo installed: its program and its 32 KiB save */
    x.size = 0x8000u;
    if (vec)
        fprintf(vec, "! prog 1 8000\n");
    CHECK(is("info", "ok v3 started 1 held 1 banks 03 kits 00 03 cap 1DFE0 used 00000 saved 0 prog 1 size 08000"));
    CHECK(is("r 8000 1", "err range"));                 /* past its save */
    x.prog = 0;
    x.size = 0x20000u;
    if (vec)
        fprintf(vec, "! prog 0 20000\n");
    CHECK(is("", "err ?"));
    CHECK(is("holdx", "err ?"));
    CHECK(is("format", "err ?"));
}

static void test_read(void)
{
    uint8_t d[4] = {0x00, 0x07, 0x0E, 0x15};
    CHECK(is("r 0 4", data(d, 4)));
    CHECK(is("r 1FFFF 1", data(sram + 0x1FFFF, 1)));
    CHECK(is("r 1ff00 100", data(sram + 0x1FF00, 256)));                /* lower-case hex, the longest reply */
    CHECK(strlen(out) == 3u + 512u + 1u + 4u);
    CHECK(is("r 1FFFF 2", "err range"));
    CHECK(is("r 20000 1", "err range"));
    CHECK(is("r 0 101", "err range"));
    CHECK(is("r 0 0", "err range"));
    CHECK(is("r FFFFFFFF 2", "err range"));             /* no wrap */
    CHECK(is("r 0", "err ?"));
    CHECK(is("r 0 4 4", "err ?"));
    CHECK(is("r 0 G", "err ?"));
    CHECK(is("r 123456789 1", "err ?"));                /* more than 8 digits */
}

static void test_write(void)
{
    static const uint8_t d[3] = {0xA1, 0xB2, 0xC3};
    char line[400], *o;
    uint32_t i;
    sprintf(line, "w 10 A1B2C3 %04X", lsdj_crc16(d, 3));
    CHECK(is(line, "ok"));
    CHECK(sram[0x10] == 0xA1 && sram[0x11] == 0xB2 && sram[0x12] == 0xC3 && sram[0x13] == (uint8_t)(0x13 * 7));
    sprintf(line, "w 20 a1b2c3 %04x", lsdj_crc16(d, 3));               /* lower case */
    CHECK(is(line, "ok") && sram[0x22] == 0xC3);
    sprintf(line, "w 30 A1B2C3 %04X", (unsigned)(lsdj_crc16(d, 3) ^ 1u));
    CHECK(is(line, "err crc") && sram[0x30] == (uint8_t)(0x30 * 7));    /* nothing written */
    CHECK(is("w 30 A1B 0000", "err hex"));
    CHECK(is("w 30 A1ZZ 0000", "err hex"));
    CHECK(is("w 30  0000", "err ?"));                   /* no data */
    sprintf(line, "w 1FFFE A1B2C3 %04X", lsdj_crc16(d, 3));
    CHECK(is(line, "err range") && sram[0x1FFFE] == (uint8_t)(0x1FFFE * 7 + 0x1FF));
    o = line + sprintf(line, "w 1FFEC ");               /* 20 bytes at the very end: the longest line */
    for (i = 0; i < LSDJ_XFER_WR; i++)
        o += sprintf(o, "%02X", (unsigned)(i ^ 0x5Au));
    {
        uint8_t b[LSDJ_XFER_WR];
        for (i = 0; i < LSDJ_XFER_WR; i++)
            b[i] = (uint8_t)(i ^ 0x5Au);
        sprintf(o, " %04X", lsdj_crc16(b, LSDJ_XFER_WR));
        CHECK(strlen("ble ") + strlen(line) <= 63u);    /* fits the console's 64-character line */
        CHECK(is(line, "ok") && sram[0x1FFEC] == 0x5A && sram[0x1FFFF] == (uint8_t)(19 ^ 0x5A));
    }
    o = line + sprintf(line, "w 100 ");                 /* 21 bytes */
    for (i = 0; i < LSDJ_XFER_WR + 1u; i++)
        o += sprintf(o, "00");
    sprintf(o, " 0000");
    CHECK(is(line, "err range") && sram[0x100] == (uint8_t)(0x100 * 7 + 1));
    o = line + sprintf(line, "w 100 ");                 /* a very long line of hex: nothing overflows */
    for (i = 0; i < 150u; i++)
        o += sprintf(o, "FF");
    CHECK(is(line, "err range"));
}

static void test_bank(void)
{
    static const uint8_t d[4] = {0xE5, 0xE6, 0xFF, 0xFF};   /* bank 1: bytes 198, 199 stored, then the fill */
    static uint8_t whole[LSDJ_BANK];
    static const uint8_t ff = 0xFF;
    char want[16];
    uint32_t i;
    CHECK(is("b 1 C6 4", data(d, 4)));
    CHECK(is("b 2 3FFF 1", data(&ff, 1)));
    CHECK(is("b 3 0 1", "err range"));
    CHECK(is("b 0 3FFF 2", "err range"));
    CHECK(is("b 0 0 101", "err range"));
    CHECK(is("b 0 0 0", "err range"));
    memset(whole, 0xFF, sizeof whole);
    for (i = 0; i < 100u; i++)
        whole[i] = (uint8_t)i;
    sprintf(want, "ok %08X", ref_crc32(whole, sizeof whole));
    CHECK(is("bc 0", want));
    CHECK(is("bc 3", "err range"));
    CHECK(is("bc", "err ?"));
}

static void test_flash(void)                  /* (started and held) the flash commands only queue: lsdj.c writes */
{
    static const uint8_t d[3] = {0x60, 0x40, 0xA1}, e = 0x77;
    char line[80];
    uint32_t i, bad = 0;
    CHECK(is("kb 0", "ok 22") && is("kb 2", "ok 24") && is("kb 3", "err range") && is("kb", "err ?"));
    CHECK(is("st", "ok 0"));
    CHECK(is("k 0", "err range") && is("kx 0", "err range"));       /* no kit bank yet */
    CHECK(is("kits 4", "err range"));                               /* the ROM has room for 3 */
    CHECK(is("kits 2", "ok") && is("st", "ok busy"));
    CHECK(is("save", "err busy") && is("kits 1", "err busy"));      /* one at a time */
    flash_state(2, 0x1BFE0u, 0, 0);
    req_done(LSDJ_REQ_KITS, 2, LSDJ_RES_OK);
    CHECK(is("st", "ok 0"));
    CHECK(is("info", "ok v3 started 1 held 1 banks 03 kits 02 03 cap 1BFE0 used 00000 saved 0 prog 0 size 20000"));
    CHECK(is("kd 0 6040 0000", "err kit") && is("kc 00000000", "err kit"));   /* no bank begun */
    CHECK(is("k 2", "err range") && is("k 1", "ok"));
    for (i = 0; i < LSDJ_BANK; i++)
        bad |= kitbuf[i] != 0xFF;
    CHECK(!bad && kitbuf[LSDJ_BANK] == 0x11);                       /* a new bank starts erased */
    sprintf(line, "kd 0 6040A1 %04X", lsdj_crc16(d, 3));
    CHECK(is(line, "ok") && !memcmp(kitbuf, d, 3) && kitbuf[3] == 0xFF);
    sprintf(line, "kd 3FFF 77 %04X", lsdj_crc16(&e, 1));
    CHECK(is(line, "ok") && kitbuf[0x3FFF] == 0x77);
    sprintf(line, "kd 3FFF 6040A1 %04X", lsdj_crc16(d, 3));
    CHECK(is(line, "err range") && kitbuf[LSDJ_BANK] == 0x11);      /* nothing past the bank */
    sprintf(line, "kd 4000 77 %04X", lsdj_crc16(&e, 1));
    CHECK(is(line, "ok") == 0 && !strcmp(out, "err range"));
    sprintf(line, "kd 10 6040A1 %04X", (unsigned)(lsdj_crc16(d, 3) ^ 1u));
    CHECK(is(line, "err crc") && kitbuf[0x10] == 0xFF);
    CHECK(is("kd 10 604 0000", "err hex") && is("kd 10", "err ?"));
    sprintf(line, "kc %08X", (unsigned)(ref_crc32(kitbuf, LSDJ_BANK) ^ 1u));
    CHECK(is(line, "err crc") && x.req == LSDJ_REQ_NONE);           /* the bank did not arrive whole: send again */
    sprintf(line, "kc %08X", (unsigned)ref_crc32(kitbuf, LSDJ_BANK));
    CHECK(is(line, "ok"));
    CHECK(is("kd 0 00 0000", "err kit"));                           /* closed: it is on its way to flash */
    req_done(LSDJ_REQ_KIT, 1, LSDJ_RES_OK);
    CHECK(is("kx 2", "err range") && is("kx 1", "ok"));
    req_done(LSDJ_REQ_KIT_ERASE, 1, LSDJ_RES_OK);
    CHECK(is("save", "ok"));
    req_done(LSDJ_REQ_SAVE, 0, LSDJ_RES_FULL);
    CHECK(is("st", "ok 1"));                                        /* the save did not fit */
    CHECK(is("save now", "err ?") && is("st 1", "err ?"));
    CHECK(is("save", "ok"));
    flash_state(2, 0x1BFE0u, 0xF00u, 1);
    req_done(LSDJ_REQ_SAVE, 0, LSDJ_RES_OK);
    CHECK(is("info", "ok v3 started 1 held 1 banks 03 kits 02 03 cap 1BFE0 used 00F00 saved 1 prog 0 size 20000"));
    CHECK(is("go", "ok"));                                          /* not held: only a save is taken */
    CHECK(is("k 1", "err hold") && is("kx 1", "err hold") && is("kits 1", "err hold") && is("kc 0", "err hold"));
    sprintf(line, "kd 0 6040A1 %04X", lsdj_crc16(d, 3));
    CHECK(is(line, "err hold"));
    CHECK(is("save", "ok"));
    req_done(LSDJ_REQ_SAVE, 0, LSDJ_RES_OK);
    CHECK(is("hold", "ok"));
}

static void test_go_and_idle(void)
{
    CHECK(x.held == 1);
    now = 1000;
    CHECK(is("st", "ok 0"));                            /* a command while held restarts the timer */
    lsdj_xfer_tick(&x, 1000 + LSDJ_XFER_IDLE_MS - 1);
    CHECK(x.held == 1);
    lsdj_xfer_tick(&x, 1000 + LSDJ_XFER_IDLE_MS);
    CHECK(x.held == 0);                                 /* the host went away: the Game Boy runs again */
    now = 0;
    CHECK(is("hold", "ok") && x.held == 1);
    CHECK(is("go", "ok") && x.held == 0 && x.reset_req == 0);
    CHECK(is("hold", "ok"));
    CHECK(is("go reset", "ok") && x.held == 0 && x.reset_req == 1);
    CHECK(is("go again", "err ?"));
    x.reset_req = 0;
    x.started = 0;
    CHECK(is("go reset", "ok") && x.reset_req == 0);    /* nothing to restart before PLAY */
}

static void test_bench(void)                  /* the test bench's commands (the page does not use them: no vectors) */
{
    x.started = 0;
    x.held = 0;
    CHECK(is("key 08", "err not started"));
    x.started = 1;
    CHECK(is("key 08", "ok") && x.key == 0x08 && x.key_n == 4);         /* START down for 4 Game Boy frames */
    CHECK(is("key 14 20", "ok") && x.key == 0x14 && x.key_n == 0x20);   /* SELECT + RIGHT for 32 */
    CHECK(is("key 100", "err range") && is("key 08 0", "err range") && is("key 08 100", "err range"));
    CHECK(x.key == 0x14 && x.key_n == 0x20);                            /* an error changes nothing */
    CHECK(is("key", "err ?") && is("key 08 4 4", "err ?"));
    x.mode = 15;
    CHECK(is("mode", "ok F") && !x.mode_set);
    CHECK(is("mode 7", "ok 7") && x.mode == 7 && x.mode_set);
    x.mode_set = 0;
    CHECK(is("mode 10", "err range") && x.mode == 7 && !x.mode_set);
}

int main(void)
{
    vec = fopen("build/tests/lsdj_xfer_vectors.txt", "w");
    setup();
    test_crc();
    test_state();
    test_read();
    test_write();
    test_bank();
    test_flash();
    test_go_and_idle();
    if (vec)
        fclose(vec);
    CHECK(vec != 0);
    vec = 0;
    test_bench();
    printf(fails ? "lsdj_xfer_test: %d FAILED\n" : "lsdj_xfer_test: all passed\n", fails);
    return fails != 0;
}
