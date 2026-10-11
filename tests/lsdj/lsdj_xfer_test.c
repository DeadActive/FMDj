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
    CHECK(is("info", "ok v1 started 0 held 0 banks 03"));
    CHECK(is("hold", "err not started"));
    CHECK(is("r 0 4", "err hold"));
    CHECK(is("w 0 00 0000", "err hold"));
    if (vec)
        fprintf(vec, "! started\n");
    x.started = 1;
    CHECK(is("  hold", "ok"));                          /* leading spaces: as the console passes the line */
    CHECK(is("info", "ok v1 started 1 held 1 banks 03"));
    CHECK(is("info x", "err ?"));
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

static void test_go_and_idle(void)
{
    CHECK(x.held == 1);
    now = 1000;
    CHECK(is("info", "ok v1 started 1 held 1 banks 03"));               /* a command while held restarts the timer */
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

int main(void)
{
    vec = fopen("build/tests/lsdj_xfer_vectors.txt", "w");
    setup();
    test_crc();
    test_state();
    test_read();
    test_write();
    test_bank();
    test_go_and_idle();
    if (vec)
        fclose(vec);
    CHECK(vec != 0);
    printf(fails ? "lsdj_xfer_test: %d FAILED\n" : "lsdj_xfer_test: all passed\n", fails);
    return fails != 0;
}
