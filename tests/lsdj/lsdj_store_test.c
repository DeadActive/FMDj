/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* Host test of the LSDj unit's flash store (firmware/src/lsdj/lsdj_store.c) on a simulated NOR flash: bits only go
 * to 0 when programmed, a sector erase sets 4 KiB to 0xFF, and the power can go in the middle of any operation.
 *   cc -Wall -Wno-unused-function -O1 -Ifirmware/src -o build/tests/lsdj_store_test tests/lsdj/lsdj_store_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lsdj/lsdj_gb.c"
#include "lsdj/lsdj_xfer.c"
#include "lsdj/lsdj_store.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define FL_N (LSDJ_ST_SECTORS * LSDJ_ST_SECTOR)
#define SAV_N 0x20000u
static uint8_t fl[FL_N];                      /* the store's area: flash offset LSDJ_ST_BASE + i */
static long ops, cut = -1;                    /* flash operations so far; the one at which the power goes (-1: never) */
static int dead;                              /* the power is off: nothing works any more */
static uint32_t erases, progs, bad;           /* bad: an operation outside the area or misaligned */

static const uint8_t *f_ptr(uint32_t off)
{
    if (off < LSDJ_ST_BASE || off >= LSDJ_ST_BASE + FL_N) {
        bad++;
        return fl;
    }
    return fl + (off - LSDJ_ST_BASE);
}

static int f_erase(uint32_t off)
{
    uint32_t i = off - LSDJ_ST_BASE;
    if (dead)
        return -1;
    if (off < LSDJ_ST_BASE || i + LSDJ_ST_SECTOR > FL_N || (off & 0xFFFu)) {
        bad++;
        return -1;
    }
    if (ops++ == cut) {                       /* half erased */
        memset(fl + i, 0xFF, LSDJ_ST_SECTOR / 2u);
        dead = 1;
        return -1;
    }
    memset(fl + i, 0xFF, LSDJ_ST_SECTOR);
    erases++;
    return 0;
}

static int f_prog(uint32_t off, const void *p, uint32_t n)
{
    uint32_t i = off - LSDJ_ST_BASE, j;
    if (dead)
        return -1;
    if (off < LSDJ_ST_BASE || i > FL_N || n > FL_N - i) {
        bad++;
        return -1;
    }
    if (ops++ == cut) {                       /* half written */
        n /= 2u;
        dead = 1;
    }
    for (j = 0; j < n; j++)
        fl[i + j] &= ((const uint8_t *)p)[j];
    if (dead)
        return -1;
    progs++;
    return 0;
}

static const lsdj_flash_t F = {f_ptr, f_erase, f_prog};
static uint8_t sav[4][SAV_N], got[SAV_N], snap[FL_N], bank[LSDJ_BANK];

static void power_on(void)
{
    dead = 0;
    cut = -1;
    ops = 0;
}

static void make_saves(void)                  /* 0: nearly empty; 1, 2: a song's worth of noise; 3: all noise */
{
    uint32_t i, s;
    srand(7);
    for (s = 0; s < 4u; s++) {
        memset(sav[s], 0, SAV_N);
        for (i = 0; i < (s == 0 ? 64u : s == 3 ? SAV_N : 0x8000u); i++)
            sav[s][i] = (uint8_t)rand();
        sav[s][0x9000u + s] = (uint8_t)(0xA0u + s);
        memset(sav[s] + 0x8141u, 0xFF, 0xBFu);
    }
}

static int load_is(uint32_t s)                /* after a fresh scan, the save in flash is sav[s] (-1: none) */
{
    lsdj_store_t st;
    lsdj_store_scan(&F, &st);
    memset(got, 0x5A, SAV_N);
    if (lsdj_store_load(&F, &st, got, SAV_N))
        return s == 0xFFu;
    return s != 0xFFu && !memcmp(got, sav[s], SAV_N);
}

typedef struct { uint8_t *p; uint32_t n; } buf_t;
static void to_buf(void *c, const uint8_t *p, uint32_t k)
{
    buf_t *b = c;
    memcpy(b->p + b->n, p, k);
    b->n += k;
}

static void test_layout(void)
{
    CHECK(sizeof(lsdj_rec_t) == 32u);
    CHECK(LSDJ_ST_BASE == 0xA0000u && LSDJ_ST_BASE + FL_N == 0xDC000u);   /* Felucca's user-sample area, no more */
    CHECK(lsdj_st_slot_sectors(0) == 30u && lsdj_st_slot_sectors(2) == 26u && lsdj_st_slot_sectors(LSDJ_ST_KMAX) == 6u);
    CHECK(lsdj_st_slot_off(0, 0) == 0xA0000u && lsdj_st_slot_off(0, 1) == 0xA0000u + 30u * 4096u);
    CHECK(lsdj_st_slot_off(2, 1) == 0xA0000u + 26u * 4096u);
    CHECK(lsdj_st_kit_off(0) == 0xDC000u - 0x4000u && lsdj_st_kit_off(1) == 0xDC000u - 0x8000u);
    CHECK(lsdj_st_kit_off(LSDJ_ST_KMAX - 1u) == 0xA0000u + 2u * 6u * 4096u);     /* right above the two slots */
    CHECK(lsdj_st_cap(0) == 30u * 4096u - 32u);
}

static void test_pack(void)
{
    static uint8_t src[70000], packed[72000], back[70000];
    static const uint32_t runs[] = {1, 2, 3, 4, 127, 128, 129, 130, 255, 256, 257, 1000};
    buf_t b = {packed, 0};
    uint32_t i, n = 0, r;
    for (r = 0; r < sizeof runs / sizeof runs[0]; r++) {     /* runs of every awkward length, a different byte between */
        memset(src + n, (int)(r + 1u), runs[r]);
        n += runs[r];
        src[n++] = (uint8_t)(0x80u + r);
    }
    srand(3);
    for (i = 0; i < 5000u; i++)
        src[n++] = (uint8_t)rand();                           /* noise: literals of 128 */
    memset(src + n, 0, 40000);                                /* a long run at the end */
    n += 40000u;
    lsdj_pack(src, n, to_buf, &b);
    CHECK(b.n == lsdj_pack_len(src, n) && b.n < n);
    CHECK(lsdj_unpack(back, n, packed, b.n) == 0 && !memcmp(back, src, n));
    b.n = 0;                                                  /* nothing packs to nothing; one byte to two */
    lsdj_pack(src, 0, to_buf, &b);
    CHECK(b.n == 0);
    lsdj_pack(src, 1, to_buf, &b);
    CHECK(b.n == 2u && lsdj_unpack(back, 1, packed, 2) == 0 && back[0] == src[0]);
    for (i = 0; i < 70000u; i++)
        src[i] = (uint8_t)rand();                             /* all noise grows by 1 in 128 at most */
    CHECK(lsdj_pack_len(src, 70000) <= 70000u + 70000u / 128u + 1u);
}

static void test_save_load(void)
{
    lsdj_store_t st;
    memset(fl, 0xFF, FL_N);
    memcpy(fl + 20u * 4096u, "FSMP", 4);                      /* Hortator's USR2 and USR3 headers from before */
    memcpy(fl + 40u * 4096u, "FSMP", 4);
    power_on();
    lsdj_store_scan(&F, &st);
    CHECK(!st.have && st.k == 0 && st.seq == 0);
    CHECK(lsdj_store_load(&F, &st, got, SAV_N) != 0 && load_is(0xFF));
    CHECK(lsdj_store_save(&F, &st, sav[0], SAV_N) == LSDJ_ST_OK);
    CHECK(st.have && st.newest == 0 && st.seq == 1 && st.len == lsdj_pack_len(sav[0], SAV_N));
    CHECK(memcmp(fl + 20u * 4096u, "FSMP", 4) && fl[40u * 4096u] == 0xFF);   /* Hortator sees those slots empty */
    CHECK(erases == 3u);                                      /* those two and the one sector a small save needs */
    CHECK(memcmp(fl, "FSMP", 4) && !memcmp(fl, "LSDS", 4));   /* USR1's header place: our record, not a sample */
    CHECK(load_is(0));
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK && st.newest == 1 && st.seq == 2 && load_is(1));
    CHECK(lsdj_store_save(&F, &st, sav[2], SAV_N) == LSDJ_ST_OK && st.newest == 0 && st.seq == 3 && load_is(2));
    lsdj_store_scan(&F, &st);                                 /* as after power-on: the newest of the two */
    CHECK(st.have && st.newest == 0 && st.seq == 3 && st.k == 0);
    CHECK(lsdj_store_save(&F, &st, sav[3], SAV_N) == LSDJ_ST_FULL && load_is(2));   /* 128 KiB of noise: over 120 KiB */
    CHECK(lsdj_store_load(&F, &st, got, SAV_N - 1u) != 0);    /* a target of another size: refused */
}

static void test_fallback(void)               /* (flash as test_save_load left it: sav[2] in A, sav[1] in B) */
{
    lsdj_store_t st;
    memcpy(snap, fl, FL_N);
    fl[100] ^= 0x10;                                          /* the newest save's data damaged: the older one */
    CHECK(load_is(1));
    lsdj_store_scan(&F, &st);
    CHECK(st.have && st.newest == 1 && st.seq == 3);          /* (the next save still counts on from the highest) */
    CHECK(lsdj_store_save(&F, &st, sav[0], SAV_N) == LSDJ_ST_OK && st.newest == 0 && st.seq == 4 && load_is(0));
    memcpy(fl, snap, FL_N);
    fl[9] ^= 1;                                               /* its record damaged: the older one */
    CHECK(load_is(1));
    memcpy(fl, snap, FL_N);
    fl[30u * 4096u + 40u] ^= 1;                               /* the older one damaged: the newest, as always */
    CHECK(load_is(2));
    fl[100] ^= 0x10;                                          /* both: none */
    CHECK(load_is(0xFF));
    memcpy(fl, snap, FL_N);
}

/* the power goes at every flash operation of a save: afterwards the flash holds the save from before, whole */
static void cuts(uint32_t before, uint32_t next)
{
    lsdj_store_t st;
    long total, c;
    uint32_t wrong = 0;
    memcpy(snap, fl, FL_N);
    power_on();
    lsdj_store_scan(&F, &st);
    CHECK(lsdj_store_save(&F, &st, sav[next], SAV_N) == LSDJ_ST_OK);
    total = ops;
    CHECK(total >= 3 && load_is(next));
    for (c = 0; c < total; c++) {
        memcpy(fl, snap, FL_N);
        power_on();
        cut = c;
        lsdj_store_scan(&F, &st);
        if (lsdj_store_save(&F, &st, sav[next], SAV_N) != LSDJ_ST_FAIL)
            wrong++;
        power_on();
        if (!load_is(before))
            wrong++;
    }
    CHECK(wrong == 0);
    memcpy(fl, snap, FL_N);
    power_on();
}

static void test_power_cut(void)
{
    memset(fl, 0xFF, FL_N);
    cuts(0xFF, 1);                                            /* the very first save: nothing, never rubbish */
    {
        lsdj_store_t st;
        lsdj_store_scan(&F, &st);
        CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK);
    }
    cuts(1, 2);                                               /* the second, into the other slot */
    {
        lsdj_store_t st;
        lsdj_store_scan(&F, &st);
        CHECK(lsdj_store_save(&F, &st, sav[2], SAV_N) == LSDJ_ST_OK);
    }
    cuts(2, 0);                                               /* the third, over the oldest */
    cuts(2, 1);
}

static void test_kits(void)
{
    lsdj_store_t st;
    uint32_t i, e;
    memset(fl, 0xFF, FL_N);
    power_on();
    lsdj_store_scan(&F, &st);
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK);
    CHECK(lsdj_store_save(&F, &st, sav[2], SAV_N) == LSDJ_ST_OK);          /* A: sav[1], B: sav[2] (newest) */
    memset(fl + (60u - 8u) * 4096u, 0x33, 8u * 4096u);        /* old data where the kit banks will be */
    for (i = 0; i < LSDJ_BANK; i++)
        bank[i] = (uint8_t)(i * 3u + 1u);
    CHECK(lsdj_store_kit_write(&F, &st, 0, bank) == LSDJ_ST_RANGE);         /* no kit bank yet */
    CHECK(lsdj_store_kits(&F, &st, 2, sav[2], SAV_N) == LSDJ_ST_OK && st.k == 2 && st.have && st.seq == 3);
    CHECK(load_is(2));
    {
        lsdj_store_t s2;
        lsdj_store_scan(&F, &s2);
        CHECK(s2.k == 2 && s2.newest == 0 && s2.seq == 3);
    }
    for (i = 0, e = 0; i < 2u * LSDJ_BANK; i++)
        e |= (uint8_t)~f_ptr(lsdj_st_kit_off(1))[i];
    CHECK(e == 0);                                            /* both new banks read erased: free to LSDj */
    CHECK(lsdj_store_kit_write(&F, &st, 1, bank) == LSDJ_ST_OK);
    CHECK(!memcmp(f_ptr(lsdj_st_kit_off(1)), bank, LSDJ_BANK) && f_ptr(lsdj_st_kit_off(0))[0] == 0xFF);
    CHECK(lsdj_store_kit_write(&F, &st, 2, bank) == LSDJ_ST_RANGE);
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK && st.newest == 1 && load_is(1));   /* B at sector 26 */
    CHECK(!memcmp(fl + 26u * 4096u, "LSDS", 4));
    CHECK(!memcmp(f_ptr(lsdj_st_kit_off(1)), bank, LSDJ_BANK));             /* a save does not touch the kits */
    fl[5] ^= 1;                                               /* slot A's record damaged: K is found from slot B */
    {
        lsdj_store_t s2;
        lsdj_store_scan(&F, &s2);
        CHECK(s2.k == 2 && s2.have && s2.newest == 1 && load_is(1));
    }
    fl[5] ^= 1;
    lsdj_store_scan(&F, &st);
    CHECK(lsdj_store_kits(&F, &st, LSDJ_ST_KMAX + 1u, sav[1], SAV_N) == LSDJ_ST_RANGE);
    memcpy(snap, fl, FL_N);
    ops = 0;                                                  /* a layout the save does not fit: nothing is touched */
    CHECK(lsdj_store_kits(&F, &st, LSDJ_ST_KMAX, sav[1], SAV_N) == LSDJ_ST_FULL && ops == 0 && !memcmp(snap, fl, FL_N));
    CHECK(st.k == 2 && load_is(1));
    CHECK(lsdj_store_kits(&F, &st, LSDJ_ST_KMAX, sav[0], SAV_N) == LSDJ_ST_OK && st.k == LSDJ_ST_KMAX && load_is(0));
    CHECK(!memcmp(f_ptr(lsdj_st_kit_off(1)), bank, LSDJ_BANK));             /* more banks: the old ones keep their kits */
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_FULL && load_is(0));   /* 24 KiB slots now */
    CHECK(lsdj_store_kit_erase(&F, &st, 1) == LSDJ_ST_OK && f_ptr(lsdj_st_kit_off(1))[0] == 0xFF && f_ptr(lsdj_st_kit_off(1))[LSDJ_BANK - 1u] == 0xFF);
    CHECK(lsdj_store_kit_write(&F, &st, 0, bank) == LSDJ_ST_OK);
    CHECK(lsdj_store_kits(&F, &st, 1, sav[1], SAV_N) == LSDJ_ST_OK && st.k == 1 && load_is(1));    /* fewer banks: room again */
    CHECK(!memcmp(f_ptr(lsdj_st_kit_off(0)), bank, LSDJ_BANK));
    memset(bank, 0xFF, LSDJ_BANK);                            /* a kit with a short tail: only its bytes are programmed */
    bank[0] = 0x60;
    bank[1] = 0x40;
    progs = 0;
    CHECK(lsdj_store_kit_write(&F, &st, 0, bank) == LSDJ_ST_OK && progs == 1u && !memcmp(f_ptr(lsdj_st_kit_off(0)), bank, LSDJ_BANK));
    cut = ops;                                                /* a flash that fails: said, not hidden */
    CHECK(lsdj_store_kit_write(&F, &st, 0, bank) == LSDJ_ST_FAIL);
    power_on();
}

int main(void)
{
    make_saves();
    test_layout();
    test_pack();
    test_save_load();
    test_fallback();
    test_power_cut();
    test_kits();
    CHECK(bad == 0);                                          /* nothing ever reached outside the area */
    printf(fails ? "lsdj_store_test: %d FAILED\n" : "lsdj_store_test: all passed\n", fails);
    return fails != 0;
}
