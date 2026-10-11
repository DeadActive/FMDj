/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj unit's flash store: LSDj's save and the custom kit banks, in Felucca's user-sample area and nowhere else
 * (flash 0xA0000..0xDBFFF, 60 sectors of 4 KiB: Hortator's projects, presets and settings are not touched; its user
 * samples are). Pure: flash is three functions (the core's, or a simulated flash in tests/lsdj/lsdj_store_test.c).
 * Include lsdj_gb.c (lsdj_unpack) and lsdj_xfer.c (lsdj_crc32_add) first.
 *
 *   kit banks: K of them (0..12), 16 KiB each, a ROM bank as it is, from the top down; an erased one reads 0xFF,
 *              which LSDj takes for a free bank
 *   the save:  two slots of 30 - 2K sectors below them, A at the bottom, B right after; a slot is a 32-byte record
 *              and the 128 KiB save PackBits-packed. A save goes to the slot that does not hold the newest one:
 *              erase, the packed bytes, read back, the record last. Until the record is whole the other slot is
 *              the save, so the power may go at any point.
 *   K changes: both slots move, so both records are erased first and the save is written again: the one step
 *              that is not safe against a power cut (the page takes a .sav to the computer first). */
#define LSDJ_ST_BASE 0xA0000u
#define LSDJ_ST_SECTOR 4096u
#define LSDJ_ST_SECTORS 60u
#define LSDJ_ST_KMAX 12u                      /* kit banks at most: the slots are then 24 KiB */
#define LSDJ_ST_MAGIC 0x5344534Cu             /* "LSDS" */
#define LSDJ_ST_VERSION 1u
enum { LSDJ_ST_OK, LSDJ_ST_FULL, LSDJ_ST_FAIL, LSDJ_ST_RANGE };   /* done; the save does not fit a slot; the flash
                                                                 * did not take it; no such kit bank */

typedef struct {
    const uint8_t *(*ptr)(uint32_t off);      /* the area as memory (not while an erase or a program runs) */
    int (*erase)(uint32_t off);               /* one 4 KiB sector; 0 = done */
    int (*prog)(uint32_t off, const void *p, uint32_t n);   /* from RAM, any length; 0 = done */
} lsdj_flash_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t k, rsv;
    uint32_t seq, len, crc;                   /* len, crc: of the packed save that follows */
    uint32_t rsv2[2];
    uint32_t hcrc;                            /* of the 28 bytes before it */
} lsdj_rec_t;

typedef struct {
    uint8_t k;                                /* kit banks */
    uint8_t have, newest;                     /* a whole save is in flash; the slot (0 = A) that holds the newest */
    uint32_t seq, len;                        /* the highest sequence number seen; the newest save's packed length */
} lsdj_store_t;

static uint32_t lsdj_st_slot_sectors(uint32_t k) { return LSDJ_ST_SECTORS / 2u - 2u * k; }
static uint32_t lsdj_st_slot_off(uint32_t k, uint32_t slot)
{
    return LSDJ_ST_BASE + slot * lsdj_st_slot_sectors(k) * LSDJ_ST_SECTOR;
}
static uint32_t lsdj_st_kit_off(uint32_t j) { return LSDJ_ST_BASE + (LSDJ_ST_SECTORS - 4u * (j + 1u)) * LSDJ_ST_SECTOR; }
static uint32_t lsdj_st_cap(uint32_t k) { return lsdj_st_slot_sectors(k) * LSDJ_ST_SECTOR - (uint32_t)sizeof(lsdj_rec_t); }

static uint32_t st_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = lsdj_crc32_add(c, *p++);
    return ~c;
}

/* PackBits, as the start-up save (tools/lsdj_rom.py packbits; lsdj_unpack reads it): the stream goes to out */
static void lsdj_pack(const uint8_t *d, uint32_t n, void (*out)(void *ctx, const uint8_t *p, uint32_t k), void *ctx)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t j = i, k = i;
        uint8_t h[2];
        while (j < n && j - i < 128u && d[j] == d[i])
            j++;
        if (j - i >= 3u) {                    /* a run */
            h[0] = (uint8_t)(0x80u | (j - i - 1u));
            h[1] = d[i];
            out(ctx, h, 2);
            i = j;
            continue;
        }
        while (k < n && k - i < 128u && !(k + 2u < n && d[k] == d[k + 1u] && d[k] == d[k + 2u]))
            k++;                              /* literal bytes up to the next run */
        h[0] = (uint8_t)(k - i - 1u);
        out(ctx, h, 1);
        out(ctx, d + i, k - i);
        i = k;
    }
}

typedef struct {
    const lsdj_flash_t *f;                    /* 0: only count */
    uint32_t off, n, len, crc;
    int err;
    uint8_t page[256];
} st_sink_t;

static void st_flush(st_sink_t *s)
{
    if (s->n && !s->err && s->f->prog(s->off, s->page, s->n))
        s->err = 1;
    s->off += s->n;
    s->n = 0;
}

static void st_out(void *ctx, const uint8_t *p, uint32_t k)
{
    st_sink_t *s = ctx;
    uint32_t i;
    for (i = 0; i < k; i++) {
        s->crc = lsdj_crc32_add(s->crc, p[i]);
        if (s->f) {
            s->page[s->n++] = p[i];
            if (s->n == sizeof s->page)
                st_flush(s);
        }
    }
    s->len += k;
}

static uint32_t lsdj_pack_len(const uint8_t *d, uint32_t n)
{
    st_sink_t s;
    s.f = 0;
    s.len = 0;
    s.crc = 0;
    lsdj_pack(d, n, st_out, &s);
    return s.len;
}

static int st_rec(const lsdj_flash_t *f, uint32_t off, lsdj_rec_t *r)    /* 1: a whole record of ours */
{
    memcpy(r, f->ptr(off), sizeof *r);
    return r->magic == LSDJ_ST_MAGIC && r->version == LSDJ_ST_VERSION && r->k <= LSDJ_ST_KMAX &&
           r->len <= lsdj_st_cap(r->k) && r->hcrc == st_crc((const uint8_t *)r, sizeof *r - 4u);
}

/* what the flash holds, as after power-on: K and the newest whole save (st->have) */
static void lsdj_store_scan(const lsdj_flash_t *f, lsdj_store_t *st)
{
    lsdj_rec_t r[2];
    int ok[2];
    uint32_t k, s, tries;
    memset(st, 0, sizeof *st);
    ok[0] = st_rec(f, LSDJ_ST_BASE, &r[0]);
    ok[1] = 0;
    if (ok[0]) {
        ok[1] = st_rec(f, lsdj_st_slot_off(r[0].k, 1), &r[1]) && r[1].k == r[0].k;
    } else {                                  /* slot A's record is gone: slot B is where its own K puts it */
        for (k = 0; k <= LSDJ_ST_KMAX; k++) {
            lsdj_rec_t t;
            if (st_rec(f, lsdj_st_slot_off(k, 1), &t) && t.k == k && (!ok[1] || t.seq > r[1].seq)) {
                r[1] = t;
                ok[1] = 1;
            }
        }
    }
    for (s = 0; s < 2u; s++)
        if (ok[s] && (r[s].seq > st->seq || !st->seq)) {
            st->seq = r[s].seq;               /* the next save counts on from the highest, */
            st->k = r[s].k;                   /* and the kit banks are where the newest record says */
        }
    for (tries = 0; tries < 2u; tries++) {    /* the newest whose packed bytes are whole */
        if (!ok[0] && !ok[1])
            return;
        s = ok[1] && (!ok[0] || r[1].seq > r[0].seq);
        if (st_crc(f->ptr(lsdj_st_slot_off(r[s].k, s) + sizeof(lsdj_rec_t)), r[s].len) == r[s].crc) {
            st->have = 1;
            st->newest = (uint8_t)s;
            st->len = r[s].len;
            return;
        }
        ok[s] = 0;
    }
}

/* the newest save into dst (n: 128 KiB); 0 = done */
static int lsdj_store_load(const lsdj_flash_t *f, const lsdj_store_t *st, uint8_t *dst, uint32_t n)
{
    if (!st->have)
        return -1;
    return lsdj_unpack(dst, n, f->ptr(lsdj_st_slot_off(st->k, st->newest) + sizeof(lsdj_rec_t)), st->len);
}

static int lsdj_store_save(const lsdj_flash_t *f, lsdj_store_t *st, const uint8_t *src, uint32_t n)
{
    st_sink_t s;
    lsdj_rec_t r;
    uint32_t slot = st->have ? !st->newest : 0u, off = lsdj_st_slot_off(st->k, slot), i, len, crc;
    s.f = 0;
    s.len = 0;
    s.crc = 0xFFFFFFFFu;
    lsdj_pack(src, n, st_out, &s);            /* first only counted: a save that does not fit writes nothing */
    len = s.len;
    crc = ~s.crc;
    if (len > lsdj_st_cap(st->k))
        return LSDJ_ST_FULL;
    if (!st->have && !st->k && !st->seq) {    /* the very first save here: Hortator's USR2 / USR3 headers go, so it
                                               * finds those slots empty, not damaged (USR1's is under our record) */
        if (f->erase(LSDJ_ST_BASE + 20u * LSDJ_ST_SECTOR) || f->erase(LSDJ_ST_BASE + 40u * LSDJ_ST_SECTOR))
            return LSDJ_ST_FAIL;
    }
    for (i = 0; i < sizeof r + len; i += LSDJ_ST_SECTOR)
        if (f->erase(off + i))
            return LSDJ_ST_FAIL;
    s.f = f;
    s.off = off + (uint32_t)sizeof r;
    s.n = 0;
    s.len = 0;
    s.crc = 0xFFFFFFFFu;
    s.err = 0;
    lsdj_pack(src, n, st_out, &s);
    st_flush(&s);
    if (s.err || st_crc(f->ptr(off + sizeof r), len) != crc)
        return LSDJ_ST_FAIL;
    memset(&r, 0, sizeof r);
    r.magic = LSDJ_ST_MAGIC;
    r.version = LSDJ_ST_VERSION;
    r.k = st->k;
    r.seq = st->seq + 1u;
    r.len = len;
    r.crc = crc;
    r.hcrc = st_crc((const uint8_t *)&r, sizeof r - 4u);
    if (f->prog(off, &r, sizeof r) || memcmp(f->ptr(off), &r, sizeof r))   /* the record: now this slot is the save */
        return LSDJ_ST_FAIL;
    st->have = 1;
    st->newest = (uint8_t)slot;
    st->seq = r.seq;
    st->len = len;
    return LSDJ_ST_OK;
}

/* K kit banks from now on, the save (src) written again in the new layout. Not safe against a power cut. */
static int lsdj_store_kits(const lsdj_flash_t *f, lsdj_store_t *st, uint32_t k, const uint8_t *src, uint32_t n)
{
    uint32_t j, i;
    if (k > LSDJ_ST_KMAX)
        return LSDJ_ST_RANGE;
    if (lsdj_pack_len(src, n) > lsdj_st_cap(k))
        return LSDJ_ST_FULL;                  /* nothing touched */
    if (f->erase(LSDJ_ST_BASE) || f->erase(lsdj_st_slot_off(st->k, 1)))   /* both records: the slots move */
        return LSDJ_ST_FAIL;
    st->have = 0;
    for (j = st->k; j < k; j++)               /* the banks that are new read erased: free */
        for (i = 0; i < LSDJ_BANK; i += LSDJ_ST_SECTOR)
            if (f->erase(lsdj_st_kit_off(j) + i))
                return LSDJ_ST_FAIL;
    st->k = (uint8_t)k;
    return lsdj_store_save(f, st, src, n);
}

static int lsdj_store_kit_erase(const lsdj_flash_t *f, const lsdj_store_t *st, uint32_t j)
{
    uint32_t i;
    if (j >= st->k)
        return LSDJ_ST_RANGE;
    for (i = 0; i < LSDJ_BANK; i += LSDJ_ST_SECTOR)
        if (f->erase(lsdj_st_kit_off(j) + i))
            return LSDJ_ST_FAIL;
    return LSDJ_ST_OK;
}

/* a kit bank (16 KiB in RAM) into flash kit bank j; its trailing 0xFF bytes are left erased */
static int lsdj_store_kit_write(const lsdj_flash_t *f, const lsdj_store_t *st, uint32_t j, const uint8_t *bank)
{
    uint32_t n = LSDJ_BANK;
    int r = lsdj_store_kit_erase(f, st, j);
    if (r)
        return r;
    while (n && bank[n - 1u] == 0xFFu)
        n--;
    if (n && f->prog(lsdj_st_kit_off(j), bank, n))
        return LSDJ_ST_FAIL;
    return memcmp(f->ptr(lsdj_st_kit_off(j)), bank, LSDJ_BANK) ? LSDJ_ST_FAIL : LSDJ_ST_OK;
}
