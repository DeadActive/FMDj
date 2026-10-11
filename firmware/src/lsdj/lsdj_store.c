/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj unit's flash store: LSDj's save and the custom kit banks, in Felucca's user-sample area and nowhere else
 * (flash 0xA0000..0xDBFFF, 60 sectors of 4 KiB: Hortator's projects, presets and settings are not touched; its user
 * samples are). Pure: flash is three functions (the core's, or a simulated flash in tests/lsdj/lsdj_store_test.c).
 * Include lsdj_gb.c (lsdj_unpack) and lsdj_xfer.c (lsdj_crc32_add) first. No name here starts with st_ or fl_:
 * gate 2b (tools/check_update_path.py) takes those for the core's storage and flash driver.
 *
 *   sectors 0, 1   the layout record, two copies: K, the number of kit banks. The newer whole copy counts.
 *   kit banks      K of them (0..12), 16 KiB each, a ROM bank as it is, from the top down; an erased one reads 0xFF,
 *                  which LSDj takes for a free bank
 *   the save       two slots of 29 - 2K sectors between them: A from sector 2 (it never moves), B right after. A
 *                  slot: a 32-byte record alone in its first page, then from byte 256 the 128 KiB save,
 *                  PackBits-packed (as storage.c lays its sectors out).
 *
 * A save goes to the slot that does not hold the newest one: erase, the packed bytes, read back, the record last.
 * Until the record is whole the other slot is the save, so the power may go at any point.
 * K changes like this: the save into slot A (so its bytes lie where every layout has slot A), the banks that are
 * new erased, then the layout record, the older copy first: that write is the change. The power may go at any point
 * of this too: K is the old or the new number, the kits that both have are untouched, a save is there. (One case
 * keeps an older save, not the newest: banks given back because the save had outgrown its slot, cut before the
 * save that follows.) */
#ifndef LSDJ_STORE_FN                         /* the functions that write flash: lsdj.c keeps them out of line, so
                                               * that gate 4 (tools/check_ble_boot.py) finds them by name */
#define LSDJ_STORE_FN static
#endif
#define LSDJ_ST_BASE 0xA0000u
#define LSDJ_ST_SECTOR 4096u
#define LSDJ_ST_SECTORS 60u
#define LSDJ_ST_KMAX 12u                      /* kit banks at most: the slots are then 20 KiB */
#define LSDJ_ST_HDR 256u                      /* a slot's first page: its record */
#define LSDJ_ST_MAGIC 0x5344534Cu             /* "LSDS": a save's record */
#define LSDJ_ST_LAYOUT 0x4C44534Cu            /* "LSDL": the layout record */
#define LSDJ_ST_VERSION 2u
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
    uint8_t rsv[2];
    uint32_t seq, len, crc;                   /* len, crc: of the packed save */
    uint32_t rsv2[2];
    uint32_t hcrc;                            /* of the 28 bytes before it */
} lsdj_rec_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t k, rsv;
    uint32_t seq;
    uint32_t rsv2[4];
    uint32_t hcrc;
} lsdj_lay_t;

typedef struct {
    uint8_t k;                                /* kit banks */
    uint8_t formatted, lay_newest;            /* a layout record is there; the copy (0 / 1) that is the newer */
    uint8_t have, newest;                     /* a whole save is in flash; the slot (0 = A) that holds the newest */
    uint32_t lay_seq, seq, len;               /* the highest sequence numbers seen; the newest save's packed length */
} lsdj_store_t;

static uint32_t lsdj_st_slot_sectors(uint32_t k) { return (LSDJ_ST_SECTORS - 2u) / 2u - 2u * k; }
static uint32_t lsdj_st_slot_off(uint32_t k, uint32_t slot)
{
    return LSDJ_ST_BASE + (2u + slot * lsdj_st_slot_sectors(k)) * LSDJ_ST_SECTOR;
}
static uint32_t lsdj_st_kit_off(uint32_t j) { return LSDJ_ST_BASE + (LSDJ_ST_SECTORS - 4u * (j + 1u)) * LSDJ_ST_SECTOR; }
static uint32_t lsdj_st_cap(uint32_t k) { return lsdj_st_slot_sectors(k) * LSDJ_ST_SECTOR - LSDJ_ST_HDR; }

static uint32_t lsdj_st_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = lsdj_crc32_add(c, *p++);
    return ~c;
}

static int lsdj_st_blank(const lsdj_flash_t *f, uint32_t off, uint32_t n)
{
    const uint8_t *p = f->ptr(off);
    while (n--)
        if (*p++ != 0xFFu)
            return 0;
    return 1;
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
    uint32_t off, n, len, max, crc;           /* max: never write more than the counting pass found */
    int err;
    uint8_t page[256];
} lsdj_st_sink_t;

static void lsdj_st_flush(lsdj_st_sink_t *s)
{
    if (s->n && !s->err && s->f->prog(s->off, s->page, s->n))
        s->err = 1;
    s->off += s->n;
    s->n = 0;
}

static void lsdj_st_out(void *ctx, const uint8_t *p, uint32_t k)
{
    lsdj_st_sink_t *s = ctx;
    uint32_t i;
    for (i = 0; i < k; i++) {
        s->crc = lsdj_crc32_add(s->crc, p[i]);
        if (s->f) {
            if (s->len + i >= s->max) {
                s->err = 1;
                break;
            }
            s->page[s->n++] = p[i];
            if (s->n == sizeof s->page)
                lsdj_st_flush(s);
        }
    }
    s->len += k;
}

static uint32_t lsdj_pack_len(const uint8_t *d, uint32_t n)
{
    lsdj_st_sink_t s;
    s.f = 0;
    s.len = 0;
    s.crc = 0;
    lsdj_pack(d, n, lsdj_st_out, &s);
    return s.len;
}

static int lsdj_st_rec(const lsdj_flash_t *f, uint32_t k, uint32_t slot, lsdj_rec_t *r)   /* 1: a whole record of ours */
{
    memcpy(r, f->ptr(lsdj_st_slot_off(k, slot)), sizeof *r);
    return r->magic == LSDJ_ST_MAGIC && r->version == LSDJ_ST_VERSION && r->len <= lsdj_st_cap(k) &&
           r->hcrc == lsdj_st_crc((const uint8_t *)r, sizeof *r - 4u);
}

/* what the flash holds, as after power-on: K, and the newest whole save (st->have) */
static void lsdj_store_scan(const lsdj_flash_t *f, lsdj_store_t *st)
{
    lsdj_rec_t r[2];
    int ok[2];
    uint32_t c, s, tries;
    memset(st, 0, sizeof *st);
    for (c = 0; c < 2u; c++) {
        lsdj_lay_t l;
        memcpy(&l, f->ptr(LSDJ_ST_BASE + c * LSDJ_ST_SECTOR), sizeof l);
        if (l.magic == LSDJ_ST_LAYOUT && l.version == LSDJ_ST_VERSION && l.k <= LSDJ_ST_KMAX &&
            l.hcrc == lsdj_st_crc((const uint8_t *)&l, sizeof l - 4u) && (!st->formatted || l.seq > st->lay_seq)) {
            st->formatted = 1;
            st->lay_newest = (uint8_t)c;
            st->lay_seq = l.seq;
            st->k = l.k;
        }
    }
    for (s = 0; s < 2u; s++) {
        ok[s] = lsdj_st_rec(f, st->k, s, &r[s]);
        if (ok[s] && r[s].seq > st->seq)
            st->seq = r[s].seq;               /* the next save counts on from the highest */
    }
    for (tries = 0; tries < 2u; tries++) {    /* the newest whose packed bytes are whole */
        if (!ok[0] && !ok[1])
            return;
        s = ok[1] && (!ok[0] || r[1].seq > r[0].seq);
        if (lsdj_st_crc(f->ptr(lsdj_st_slot_off(st->k, s) + LSDJ_ST_HDR), r[s].len) == r[s].crc) {
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
    return lsdj_unpack(dst, n, f->ptr(lsdj_st_slot_off(st->k, st->newest) + LSDJ_ST_HDR), st->len);
}

/* the layout record with K = k: the older copy first (that write is the change), then the other, so that one
 * damaged copy later does not bring an old K back */
static int lsdj_st_layout(const lsdj_flash_t *f, lsdj_store_t *st, uint32_t k)
{
    uint32_t c = st->formatted ? !st->lay_newest : 0u, i;
    for (i = 0; i < 2u; i++, c ^= 1u) {
        lsdj_lay_t l;
        uint32_t off = LSDJ_ST_BASE + c * LSDJ_ST_SECTOR;
        memset(&l, 0, sizeof l);
        l.magic = LSDJ_ST_LAYOUT;
        l.version = LSDJ_ST_VERSION;
        l.k = (uint8_t)k;
        l.seq = st->lay_seq + 1u;
        l.hcrc = lsdj_st_crc((const uint8_t *)&l, sizeof l - 4u);
        if (f->erase(off) || f->prog(off, &l, sizeof l) || memcmp(f->ptr(off), &l, sizeof l))
            return LSDJ_ST_FAIL;
        st->formatted = 1;
        st->lay_newest = (uint8_t)c;
        st->lay_seq = l.seq;
        st->k = (uint8_t)k;
    }
    return LSDJ_ST_OK;
}

static int lsdj_st_save(const lsdj_flash_t *f, lsdj_store_t *st, const uint8_t *src, uint32_t n)
{
    lsdj_st_sink_t s;
    lsdj_rec_t r;
    uint32_t slot, off, i, len, crc;
    s.f = 0;
    s.len = 0;
    s.crc = 0xFFFFFFFFu;
    lsdj_pack(src, n, lsdj_st_out, &s);       /* first only counted: a save that does not fit writes nothing */
    len = s.len;
    crc = ~s.crc;
    if (len > lsdj_st_cap(st->k))
        return LSDJ_ST_FULL;
    if (!st->formatted) {
        if (!st->seq) {                       /* nothing of ours here yet: Hortator's USR2 / USR3 headers go, so it
                                               * finds those slots empty, not damaged (USR1's is under the layout
                                               * record). Never with a save or a layout record there: K is then 0
                                               * for certain, and these two sectors are in no kit bank. */
            if (f->erase(LSDJ_ST_BASE + 20u * LSDJ_ST_SECTOR) || f->erase(LSDJ_ST_BASE + 40u * LSDJ_ST_SECTOR))
                return LSDJ_ST_FAIL;
        }
        if (lsdj_st_layout(f, st, 0))
            return LSDJ_ST_FAIL;
    }
    slot = st->have ? !st->newest : 0u;
    off = lsdj_st_slot_off(st->k, slot);
    for (i = 0; i < LSDJ_ST_HDR + len; i += LSDJ_ST_SECTOR)
        if (f->erase(off + i))
            return LSDJ_ST_FAIL;
    s.f = f;
    s.off = off + LSDJ_ST_HDR;
    s.n = 0;
    s.len = 0;
    s.max = len;
    s.crc = 0xFFFFFFFFu;
    s.err = 0;
    lsdj_pack(src, n, lsdj_st_out, &s);
    lsdj_st_flush(&s);
    if (s.err || s.len != len || lsdj_st_crc(f->ptr(off + LSDJ_ST_HDR), len) != crc)
        return LSDJ_ST_FAIL;
    memset(&r, 0, sizeof r);
    r.magic = LSDJ_ST_MAGIC;
    r.version = LSDJ_ST_VERSION;
    r.seq = st->seq + 1u;
    r.len = len;
    r.crc = crc;
    r.hcrc = lsdj_st_crc((const uint8_t *)&r, sizeof r - 4u);
    if (f->prog(off, &r, sizeof r) || memcmp(f->ptr(off), &r, sizeof r))   /* the record: now this slot is the save */
        return LSDJ_ST_FAIL;
    st->have = 1;
    st->newest = (uint8_t)slot;
    st->seq = r.seq;
    st->len = len;
    return LSDJ_ST_OK;
}

/* the save (src: the 128 KiB save RAM) into flash. After a flash error st is read from the flash again: what is
 * there counts, not what was meant to be. */
LSDJ_STORE_FN int lsdj_store_save(const lsdj_flash_t *f, lsdj_store_t *st, const uint8_t *src, uint32_t n)
{
    int r = lsdj_st_save(f, st, src, n);
    if (r == LSDJ_ST_FAIL)
        lsdj_store_scan(f, st);
    return r;
}

static int lsdj_st_kits(const lsdj_flash_t *f, lsdj_store_t *st, uint32_t k, const uint8_t *src, uint32_t n)
{
    lsdj_rec_t t;
    uint32_t j, i, in_a;
    int r = LSDJ_ST_OK;
    /* 1. the save as it is now into slot A: every layout has slot A in the same place, and what fits the new layout
     *    ends below its slot B and its banks. (Banks given back because the save had outgrown its slot: it does not
     *    fit yet, and slot A keeps what it has.) */
    if (st->have && st->newest == 0)
        r = lsdj_st_save(f, st, src, n);      /* into B, so that the next one goes to A */
    if (r == LSDJ_ST_OK)
        r = lsdj_st_save(f, st, src, n);
    if (r == LSDJ_ST_FAIL)
        return r;
    in_a = r == LSDJ_ST_OK;
    if (k > st->k && !in_a)
        return LSDJ_ST_FAIL;                  /* (cannot be: what fits fewer sectors fits more) */
    /* 2. the banks that are new read erased: free. They lie in the old slot B or above the new slot A's end */
    for (j = st->k; j < k; j++)
        for (i = 0; i < LSDJ_BANK; i += LSDJ_ST_SECTOR)
            if (!lsdj_st_blank(f, lsdj_st_kit_off(j) + i, LSDJ_ST_SECTOR) && f->erase(lsdj_st_kit_off(j) + i))
                return LSDJ_ST_FAIL;
    /* 3. the change */
    if (lsdj_st_layout(f, st, k))
        return LSDJ_ST_FAIL;
    /* 4. a record from an older layout where slot B begins now: gone, then see what is there */
    if (lsdj_st_rec(f, k, 1, &t) && f->erase(lsdj_st_slot_off(k, 1)))
        return LSDJ_ST_FAIL;
    lsdj_store_scan(f, st);
    return in_a ? LSDJ_ST_OK : lsdj_st_save(f, st, src, n);
}

/* K kit banks from now on (src: the save RAM, which must fit the new slots). See the top for what a power cut
 * leaves. */
LSDJ_STORE_FN int lsdj_store_kits(const lsdj_flash_t *f, lsdj_store_t *st, uint32_t k, const uint8_t *src, uint32_t n)
{
    int r;
    if (k > LSDJ_ST_KMAX)
        return LSDJ_ST_RANGE;
    if (lsdj_pack_len(src, n) > lsdj_st_cap(k))
        return LSDJ_ST_FULL;                  /* nothing touched */
    if (k == st->k)
        return LSDJ_ST_OK;
    r = lsdj_st_kits(f, st, k, src, n);
    if (r == LSDJ_ST_FAIL)
        lsdj_store_scan(f, st);
    return r;
}

LSDJ_STORE_FN int lsdj_store_kit_erase(const lsdj_flash_t *f, const lsdj_store_t *st, uint32_t j)
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
LSDJ_STORE_FN int lsdj_store_kit_write(const lsdj_flash_t *f, const lsdj_store_t *st, uint32_t j, const uint8_t *bank)
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
