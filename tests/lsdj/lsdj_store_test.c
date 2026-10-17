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
static int cut_mode;                          /* how it goes: 0 the operation half done, 1 not begun, 2 done but not reported */
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
    if (ops++ == cut) {
        memset(fl + i, 0xFF, cut_mode == 0 ? LSDJ_ST_SECTOR / 2u : cut_mode == 1 ? 0u : LSDJ_ST_SECTOR);
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
    if (ops++ == cut) {
        n = cut_mode == 0 ? n / 2u : cut_mode == 1 ? 0u : n;
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
static uint8_t sav[5][SAV_N], got[SAV_N], snap[FL_N], bank[LSDJ_BANK];

static void power_on(void)
{
    dead = 0;
    cut = -1;
    ops = 0;
}

static void make_saves(void)                  /* 0: nearly empty; 1, 2: a song's worth of noise; 3: all noise; 4: 96 KiB */
{
    uint32_t i, s;
    srand(7);
    for (s = 0; s < 5u; s++) {
        memset(sav[s], 0, SAV_N);
        for (i = 0; i < (s == 0 ? 64u : s == 3 ? SAV_N : s == 4 ? 0x18000u : 0x8000u); i++)
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
    uint32_t k;
    CHECK(sizeof(lsdj_rec_t) == 32u && sizeof(lsdj_lay_t) == 32u);
    CHECK(LSDJ_ST_BASE == 0xA0000u && LSDJ_ST_BASE + FL_N == 0xDC000u);   /* Felucca's user-sample area, no more */
    CHECK(lsdj_st_slot_sectors(0) == 29u && lsdj_st_slot_sectors(2) == 25u && lsdj_st_slot_sectors(LSDJ_ST_KMAX) == 5u);
    CHECK(lsdj_st_slot_off(0, 0) == 0xA0000u + 2u * 4096u && lsdj_st_slot_off(0, 1) == 0xA0000u + 31u * 4096u);
    CHECK(lsdj_st_slot_off(7, 0) == lsdj_st_slot_off(0, 0));            /* slot A never moves */
    CHECK(lsdj_st_kit_off(0) == 0xDC000u - 0x4000u && lsdj_st_kit_off(1) == 0xDC000u - 0x8000u);
    CHECK(lsdj_st_cap(0) == 29u * 4096u - 256u);
    for (k = 0; k <= LSDJ_ST_KMAX; k++) {                               /* two layout sectors, two slots, K banks: 60 */
        CHECK(lsdj_st_slot_off(k, 1) + lsdj_st_slot_sectors(k) * 4096u == (k ? lsdj_st_kit_off(k - 1u) : 0xDC000u));
        CHECK(lsdj_st_slot_off(k, 0) + lsdj_st_slot_sectors(k) * 4096u == lsdj_st_slot_off(k, 1));
    }
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

#define A0 (2u * 4096u)                       /* slot A in fl[]; slot B with no kit banks */
#define B0 (31u * 4096u)

static void test_save_load(void)
{
    lsdj_store_t st;
    memset(fl, 0xFF, FL_N);
    memcpy(fl, "FSMP", 4);                                    /* Hortator's USR1..USR3 headers from before */
    memcpy(fl + 20u * 4096u, "FSMP", 4);
    memcpy(fl + 40u * 4096u, "FSMP", 4);
    power_on();
    lsdj_store_scan(&F, &st);
    CHECK(!st.have && !st.formatted && st.k == 0 && st.seq == 0);
    CHECK(lsdj_store_load(&F, &st, got, SAV_N) != 0 && load_is(0xFF));
    CHECK(lsdj_store_save(&F, &st, sav[0], SAV_N) == LSDJ_ST_OK);
    CHECK(st.have && st.formatted && st.newest == 0 && st.seq == 1 && st.len == lsdj_pack_len(sav[0], SAV_N));
    CHECK(memcmp(fl, "FSMP", 4) && memcmp(fl + 20u * 4096u, "FSMP", 4) && fl[40u * 4096u] == 0xFF);   /* Hortator: empty */
    CHECK(erases == 5u);                                      /* those two, the layout record's two copies, one for a small save */
    CHECK(!memcmp(fl + 4096u, "LSDL", 4));
    CHECK(!memcmp(fl, "LSDL", 4) && !memcmp(fl + A0, "LSDS", 4) && fl[A0 + 32u] == 0xFF && fl[A0 + 255u] == 0xFF);
    CHECK(load_is(0));
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK && st.newest == 1 && st.seq == 2 && load_is(1));
    CHECK(!memcmp(fl + B0, "LSDS", 4));
    CHECK(lsdj_store_save(&F, &st, sav[2], SAV_N) == LSDJ_ST_OK && st.newest == 0 && st.seq == 3 && load_is(2));
    lsdj_store_scan(&F, &st);                                 /* as after power-on: the newest of the two */
    CHECK(st.have && st.newest == 0 && st.seq == 3 && st.k == 0 && st.formatted);
    ops = 0;
    CHECK(lsdj_store_save(&F, &st, sav[3], SAV_N) == LSDJ_ST_FULL && ops == 0 && load_is(2));   /* 128 KiB of noise */
    CHECK(lsdj_store_load(&F, &st, got, SAV_N - 1u) != 0);    /* a target of another size: refused */
}

static void test_fallback(void)               /* (flash as test_save_load left it: sav[2] in A, sav[1] in B) */
{
    lsdj_store_t st;
    memcpy(snap, fl, FL_N);
    fl[A0 + 300u] ^= 0x10;                                    /* the newest save's data damaged: the older one */
    CHECK(load_is(1));
    lsdj_store_scan(&F, &st);
    CHECK(st.have && st.newest == 1 && st.seq == 3);          /* (the next save still counts on from the highest) */
    CHECK(lsdj_store_save(&F, &st, sav[0], SAV_N) == LSDJ_ST_OK && st.newest == 0 && st.seq == 4 && load_is(0));
    memcpy(fl, snap, FL_N);
    fl[A0 + 9u] ^= 1;                                         /* its record damaged: the older one */
    CHECK(load_is(1));
    memcpy(fl, snap, FL_N);
    fl[B0 + 300u] ^= 1;                                       /* the older one damaged: the newest, as always */
    CHECK(load_is(2));
    fl[A0 + 300u] ^= 0x10;                                    /* both: none */
    CHECK(load_is(0xFF));
    memcpy(fl, snap, FL_N);
    fl[9] ^= 1;                                               /* one copy of the layout record damaged: the other */
    lsdj_store_scan(&F, &st);
    CHECK(st.formatted && st.k == 0 && st.have && load_is(2));
    fl[4096u + 9u] ^= 1;                                      /* both: K reads 0; the saves are found as before */
    lsdj_store_scan(&F, &st);
    CHECK(!st.formatted && st.k == 0 && st.have && load_is(2));
    memcpy(fl + 20u * 4096u, "MARK", 4);                      /* (NOR: bits to 0 only; this place was erased) */
    CHECK(lsdj_store_save(&F, &st, sav[0], SAV_N) == LSDJ_ST_OK && st.formatted && load_is(0));   /* written again, and */
    CHECK(!memcmp(fl + 20u * 4096u, "MARK", 4));              /* with saves there, nothing else is erased */
    fl[B0 + 300u] ^= 1;                                       /* (the save just made is in B: without it, the one before) */
    CHECK(load_is(2));
    memcpy(fl, snap, FL_N);
}

/* the power goes at every flash operation of a save: afterwards the flash holds the save from before, whole */
static void cuts(uint32_t before, uint32_t next)
{
    lsdj_store_t st;
    long total, c;
    uint32_t wrong = 0;
    for (cut_mode = 0; cut_mode < 3; cut_mode++) {
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
        if (!load_is(cut_mode == 2 && c == total - 1 ? next : before))   /* (the last operation done: the new save) */
            wrong++;
        lsdj_store_scan(&F, &st);             /* and the store works on: the next save is found */
        if (lsdj_store_save(&F, &st, sav[0], SAV_N) != LSDJ_ST_OK || !load_is(0))
            wrong++;
    }
    CHECK(wrong == 0);
    memcpy(fl, snap, FL_N);
    power_on();
    }
    cut_mode = 0;
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

static uint32_t blank(uint32_t off, uint32_t n)   /* that stretch of flash reads erased */
{
    uint32_t i, e = 0;
    for (i = 0; i < n; i++)
        e |= (uint8_t)~f_ptr(off)[i];
    return !e;
}

static void test_kits(void)
{
    lsdj_store_t st;
    uint32_t i;
    memset(fl, 0xFF, FL_N);
    power_on();
    lsdj_store_scan(&F, &st);
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK);
    CHECK(lsdj_store_save(&F, &st, sav[2], SAV_N) == LSDJ_ST_OK);          /* A: sav[1], B: sav[2] (newest) */
    for (i = 0; i < LSDJ_BANK; i++)
        bank[i] = (uint8_t)(i * 3u + 1u);
    CHECK(lsdj_store_kit_write(&F, &st, 0, bank) == LSDJ_ST_RANGE);         /* no kit bank yet */
    CHECK(lsdj_store_kits(&F, &st, 2, sav[2], SAV_N) == LSDJ_ST_OK && st.k == 2 && st.have && load_is(2));
    {
        lsdj_store_t s2;
        lsdj_store_scan(&F, &s2);
        CHECK(s2.k == 2 && s2.have && s2.newest == 0);                      /* the save is in the slot that did not move */
    }
    CHECK(blank(lsdj_st_kit_off(1), 2u * LSDJ_BANK));         /* both new banks read erased: free to LSDj */
    ops = 0;
    CHECK(lsdj_store_kits(&F, &st, 2, sav[2], SAV_N) == LSDJ_ST_OK && ops == 0);   /* the same number: nothing to do */
    CHECK(lsdj_store_kit_write(&F, &st, 1, bank) == LSDJ_ST_OK);
    CHECK(!memcmp(f_ptr(lsdj_st_kit_off(1)), bank, LSDJ_BANK) && f_ptr(lsdj_st_kit_off(0))[0] == 0xFF);
    CHECK(lsdj_store_kit_write(&F, &st, 2, bank) == LSDJ_ST_RANGE);
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK && st.newest == 1 && load_is(1));
    CHECK(!memcmp(fl + 27u * 4096u, "LSDS", 4));              /* slot B with two kit banks: sector 2 + 25 */
    CHECK(!memcmp(f_ptr(lsdj_st_kit_off(1)), bank, LSDJ_BANK));             /* a save does not touch the kits */
    fl[A0 + 5u] ^= 1;                                         /* slot A's record damaged: K is still known */
    {
        lsdj_store_t s2;
        lsdj_store_scan(&F, &s2);
        CHECK(s2.k == 2 && s2.have && s2.newest == 1 && load_is(1));
    }
    fl[A0 + 5u] ^= 1;
    fl[A0 + 300u] ^= 1;                                       /* both saves damaged: no save, the kit banks as they were */
    fl[27u * 4096u + 300u] ^= 1;
    {
        lsdj_store_t s2;
        lsdj_store_scan(&F, &s2);
        CHECK(s2.k == 2 && !s2.have && s2.formatted);
        CHECK(lsdj_store_save(&F, &s2, sav[0], SAV_N) == LSDJ_ST_OK && load_is(0));
        CHECK(!memcmp(f_ptr(lsdj_st_kit_off(1)), bank, LSDJ_BANK));         /* a first save there erases no kit */
    }
    lsdj_store_scan(&F, &st);
    CHECK(lsdj_store_kits(&F, &st, LSDJ_ST_KMAX + 1u, sav[1], SAV_N) == LSDJ_ST_RANGE);
    memcpy(snap, fl, FL_N);
    ops = 0;                                                  /* a layout the save does not fit: nothing is touched */
    CHECK(lsdj_store_kits(&F, &st, LSDJ_ST_KMAX, sav[1], SAV_N) == LSDJ_ST_FULL && ops == 0 && !memcmp(snap, fl, FL_N));
    CHECK(st.k == 2 && load_is(0));
    CHECK(lsdj_store_kits(&F, &st, LSDJ_ST_KMAX, sav[0], SAV_N) == LSDJ_ST_OK && st.k == LSDJ_ST_KMAX && load_is(0));
    CHECK(!memcmp(f_ptr(lsdj_st_kit_off(1)), bank, LSDJ_BANK));             /* more banks: the old ones keep their kits */
    CHECK(blank(lsdj_st_kit_off(LSDJ_ST_KMAX - 1u), 10u * LSDJ_BANK));      /* the ten new ones are free */
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_FULL && load_is(0));   /* 20 KiB slots now */
    CHECK(lsdj_store_kit_erase(&F, &st, 1) == LSDJ_ST_OK && blank(lsdj_st_kit_off(1), LSDJ_BANK));
    CHECK(lsdj_store_kit_write(&F, &st, 0, bank) == LSDJ_ST_OK);
    /* banks given back because the save had outgrown its slot: the save in RAM (sav[1]) fits only afterwards */
    CHECK(lsdj_store_kits(&F, &st, 1, sav[1], SAV_N) == LSDJ_ST_OK && st.k == 1 && load_is(1));
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

/* the power goes at every flash operation while the number of kit banks changes from k0 to k1 (the save in RAM:
 * sav[now]; in flash before: sav[was]). Afterwards: K is the old or the new number, a save loads (the one from
 * before, or the one in RAM), every kit bank that both layouts have still holds its kit, and the store works on */
static void kit_cuts(uint32_t k0, uint32_t k1, uint32_t was, uint32_t now)
{
    lsdj_store_t st;
    long total, c;
    uint32_t wrong = 0, j, keep = k0 < k1 ? k0 : k1;
    memset(fl, 0xFF, FL_N);
    power_on();
    lsdj_store_scan(&F, &st);
    CHECK(lsdj_store_save(&F, &st, sav[0], SAV_N) == LSDJ_ST_OK);
    CHECK(lsdj_store_kits(&F, &st, k0, sav[0], SAV_N) == LSDJ_ST_OK);
    for (j = 0; j < k0; j++) {
        memset(bank, (int)(0x40u + j), LSDJ_BANK);
        CHECK(lsdj_store_kit_write(&F, &st, j, bank) == LSDJ_ST_OK);
    }
    CHECK(lsdj_store_save(&F, &st, sav[was], SAV_N) == LSDJ_ST_OK);
    CHECK(lsdj_store_save(&F, &st, sav[was], SAV_N) == LSDJ_ST_OK && load_is(was));
    memcpy(snap, fl, FL_N);
    for (cut_mode = 0; cut_mode < 3; cut_mode++) {
        power_on();
        lsdj_store_scan(&F, &st);
        CHECK(lsdj_store_kits(&F, &st, k1, sav[now], SAV_N) == LSDJ_ST_OK && st.k == k1 && load_is(now));
        total = ops;
        for (c = 0; c < total; c++) {
            lsdj_store_t s2;
            memcpy(fl, snap, FL_N);
            power_on();
            cut = c;
            lsdj_store_scan(&F, &st);
            lsdj_store_kits(&F, &st, k1, sav[now], SAV_N);
            power_on();
            lsdj_store_scan(&F, &s2);
            if ((s2.k != k0 && s2.k != k1) || !s2.have || !(load_is(was) || load_is(now)))
                wrong++;
            for (j = 0; j < keep; j++)
                if (f_ptr(lsdj_st_kit_off(j))[0] != 0x40u + j || f_ptr(lsdj_st_kit_off(j))[LSDJ_BANK - 1u] != 0x40u + j)
                    wrong++;
            if (s2.k == k1 && k1 > k0 && !blank(lsdj_st_kit_off(k1 - 1u), (k1 - k0) * LSDJ_BANK))
                wrong++;                      /* the new layout never shows a new bank that is not free */
            if (lsdj_store_save(&F, &s2, sav[0], SAV_N) != LSDJ_ST_OK || !load_is(0))
                wrong++;
        }
        memcpy(fl, snap, FL_N);
    }
    cut_mode = 0;
    power_on();
    CHECK(wrong == 0);
}

static void test_kit_cuts(void)
{
    kit_cuts(0, 1, 1, 1);                                     /* the first kit bank */
    kit_cuts(1, 2, 1, 2);
    kit_cuts(5, 6, 0, 0);
    kit_cuts(0, 12, 0, 0);                                    /* far: the new banks reach where slot A used to end */
    kit_cuts(2, 1, 1, 2);                                     /* one given back */
    kit_cuts(12, 3, 0, 1);                                    /* given back because the save in RAM had outgrown the slot */
    kit_cuts(6, 0, 0, 2);
    kit_cuts(1, 2, 4, 4);                                     /* a big save: its bytes in slot B lie where the new bank goes */
    kit_cuts(0, 12, 4, 0);                                    /* big saves in both slots, a small one in RAM: the new banks
                                                               * cover both of the old ones */
}

static void test_cuts_with_kits(void)         /* a save's power cuts with kit banks in place: they are never touched */
{
    static const uint32_t KS[3] = {1, 6, 12};
    lsdj_store_t st;
    uint32_t i, j;
    for (i = 0; i < 3u; i++) {
        memset(fl, 0xFF, FL_N);
        power_on();
        lsdj_store_scan(&F, &st);
        CHECK(lsdj_store_kits(&F, &st, KS[i], sav[0], SAV_N) == LSDJ_ST_OK);
        for (j = 0; j < KS[i]; j++) {
            memset(bank, (int)(0x40u + j), LSDJ_BANK);
            CHECK(lsdj_store_kit_write(&F, &st, j, bank) == LSDJ_ST_OK);
        }
        cuts(0, 0);
        for (j = 0; j < KS[i]; j++)
            CHECK(f_ptr(lsdj_st_kit_off(j))[0] == 0x40u + j && f_ptr(lsdj_st_kit_off(j))[LSDJ_BANK - 1u] == 0x40u + j);
    }
}

/* the program a save is of (LSDj or DMGo, lsdj_gb.c lsdj_prog): kept in its record; a record without it (every
 * save before DMGo) is LSDj's */
static void test_prog(void)
{
    lsdj_store_t st;
    lsdj_rec_t r;
    memset(fl, 0xFF, sizeof fl);
    power_on();
    lsdj_store_scan(&F, &st);
    CHECK(!st.have && st.prog == LSDJ_PROG_LSDJ);
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK);   /* st.prog as scanned: LSDj */
    memcpy(&r, f_ptr(lsdj_st_slot_off(st.k, st.newest)), sizeof r);
    CHECK(r.prog == 0 && r.rsv == 0);                         /* as the records before it: their reserved bytes */
    lsdj_store_scan(&F, &st);
    CHECK(st.have && st.prog == LSDJ_PROG_LSDJ);
    st.prog = LSDJ_PROG_DMGO;                                 /* DMGo installed: REC saves its 32 KiB */
    CHECK(lsdj_store_save(&F, &st, sav[2], 0x8000u) == LSDJ_ST_OK && st.prog == LSDJ_PROG_DMGO);
    lsdj_store_scan(&F, &st);
    CHECK(st.have && st.prog == LSDJ_PROG_DMGO);
    memset(got, 0x5A, SAV_N);
    CHECK(lsdj_store_load(&F, &st, got, 0x8000u) == 0 && !memcmp(got, sav[2], 0x8000u));
    CHECK(lsdj_store_load(&F, &st, got, SAV_N) != 0);         /* not 128 KiB */
    CHECK(lsdj_store_kits(&F, &st, 2, sav[2], 0x8000u) == LSDJ_ST_OK);   /* the save moved by a new layout: still DMGo's */
    lsdj_store_scan(&F, &st);
    CHECK(st.have && st.k == 2 && st.prog == LSDJ_PROG_DMGO);
    st.prog = LSDJ_PROG_LSDJ;                                 /* LSDj again: REC makes it LSDj's */
    CHECK(lsdj_store_save(&F, &st, sav[1], SAV_N) == LSDJ_ST_OK);
    lsdj_store_scan(&F, &st);
    CHECK(st.prog == LSDJ_PROG_LSDJ && load_is(1));
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
    test_kit_cuts();
    test_cuts_with_kits();
    test_prog();                              /* (last: it starts from an empty area) */
    CHECK(bad == 0);                                          /* nothing ever reached outside the area */
    printf(fails ? "lsdj_store_test: %d FAILED\n" : "lsdj_store_test: all passed\n", fails);
    return fails != 0;
}
