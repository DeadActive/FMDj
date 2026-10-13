/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The cartridge block: the user's own LSDj ROM and its start-up save as one piece of data, put after the firmware's
 * own bytes at the very end of the app's flash slot. The firmware holds nothing of LSDj: the install page
 * (web/lsdj_install.html, this code as WebAssembly) and the Mac build (tools/lsdj_cart_tool.c) make the block from a
 * ROM file, and the LSDj unit reads it on PLAY. Pure (no core, no hardware; tests/lsdj/lsdj_cart_test.c). Include
 * lsdj_gb.c first.
 *
 *   body:    the banks' lengths, 16 bits each (a bank is stored without its trailing 0xFF bytes: lsdj_bank_t)
 *            the packed save's length, 32 bits; the save, PackBits (lsdj_unpack gives its 128 KiB)
 *            the banks' bytes, one after the other
 *   trailer: "LSDC", version (16 bits), banks (16 bits), the body's length, the body's CRC-32: 16 bytes, the last
 *            of the slot
 * Numbers are little endian. */
#define LSDJ_CART_VERSION 1u
#define LSDJ_CART_TRAILER 16u
#define LSDJ_CART_BANKS 64u                   /* 1 MiB: the most a ROM has */
#define LSDJ_CART_SAVE (128u * 1024u)
enum { LSDJ_CART_OK, LSDJ_CART_NONE = -1,     /* no block here */
       LSDJ_CART_BAD = -2,                    /* a block, but damaged or not for this firmware */
       LSDJ_CART_ROM_SIZE = -3,               /* the file is not 1 .. 64 whole banks */
       LSDJ_CART_ROM_TYPE = -4,               /* not MBC5 + RAM + battery with a 128 KiB save: not LSDj */
       LSDJ_CART_ROM_SUM = -5,                /* the header checksum is wrong */
       LSDJ_CART_FULL = -6 };                 /* the block does not fit */

typedef struct {
    uint32_t nbanks;
    lsdj_bank_t bank[LSDJ_CART_BANKS];
    const uint8_t *save;                      /* packed */
    uint32_t save_len;
} lsdj_cart_t;

static uint32_t lsdj_cart_u16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t lsdj_cart_u32(const uint8_t *p) { return lsdj_cart_u16(p) | lsdj_cart_u16(p + 2) << 16; }
static uint32_t lsdj_cart_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = lsdj_crc32_add(c, *p++);
    return ~c;
}

/* end: one past the block's last byte (the end of the slot); room: the bytes before `end` that may be read.
 * LSDJ_CART_OK and *c (pointers into the block), LSDJ_CART_NONE or LSDJ_CART_BAD */
static int lsdj_cart_read(const uint8_t *end, uint32_t room, lsdj_cart_t *c)
{
    const uint8_t *t, *body, *p;
    uint32_t n, len, i, at;
    if (room < LSDJ_CART_TRAILER)
        return LSDJ_CART_NONE;
    t = end - LSDJ_CART_TRAILER;
    if (t[0] != 'L' || t[1] != 'S' || t[2] != 'D' || t[3] != 'C')
        return LSDJ_CART_NONE;
    n = lsdj_cart_u16(t + 6);
    len = lsdj_cart_u32(t + 8);
    if (lsdj_cart_u16(t + 4) != LSDJ_CART_VERSION || !n || n > LSDJ_CART_BANKS || len > room - LSDJ_CART_TRAILER ||
        len < 2u * n + 4u)
        return LSDJ_CART_BAD;
    body = t - len;
    if (lsdj_cart_crc(body, len) != lsdj_cart_u32(t + 12))
        return LSDJ_CART_BAD;
    c->nbanks = n;
    c->save_len = lsdj_cart_u32(body + 2u * n);
    at = 2u * n + 4u;
    if (c->save_len > len - at)
        return LSDJ_CART_BAD;
    c->save = body + at;
    at += c->save_len;
    for (i = 0, p = body; i < LSDJ_CART_BANKS; i++, p += 2) {
        uint32_t k = i < n ? lsdj_cart_u16(p) : 0;
        if (k > LSDJ_BANK || k > len - at)
            return LSDJ_CART_BAD;
        c->bank[i].p = k ? body + at : 0;
        c->bank[i].n = (uint16_t)k;
        at += k;
    }
    return at == len ? LSDJ_CART_OK : LSDJ_CART_BAD;
}

#ifdef LSDJ_CART_MAKE                         /* the makers only: the page and the Mac build */
/* LSDJ_CART_OK, or why the file cannot be an LSDj ROM (as tools/lsdj_rom.py checked it, and emu_init) */
static int lsdj_cart_rom_check(const uint8_t *rom, uint32_t len)
{
    uint32_t i;
    uint8_t x = 0;
    if (!len || len % LSDJ_BANK || len > LSDJ_CART_BANKS * LSDJ_BANK)
        return LSDJ_CART_ROM_SIZE;
    if (rom[0x147] != 0x1Bu || rom[0x149] != 0x04u)
        return LSDJ_CART_ROM_TYPE;
    for (i = 0x134; i <= 0x14Cu; i++)
        x = (uint8_t)(x - rom[i] - 1u);
    return x == rom[0x14D] ? LSDJ_CART_OK : LSDJ_CART_ROM_SUM;
}

static uint32_t lsdj_cart_trim(const uint8_t *bank)      /* a bank's length without its trailing 0xFF bytes */
{
    uint32_t k = LSDJ_BANK;
    while (k && bank[k - 1u] == 0xFFu)
        k--;
    return k;
}

/* PackBits (lsdj_unpack's): out = 0 only counts. Runs of 3 or more are runs */
static uint32_t lsdj_cart_pack(uint8_t *out, const uint8_t *d, uint32_t n)
{
    uint32_t o = 0, i = 0, j, k;
    while (i < n) {
        for (j = i; j < n && j - i < 128u && d[j] == d[i]; j++)
            ;
        if (j - i >= 3u) {
            if (out) {
                out[o] = (uint8_t)(0x80u | (j - i - 1u));
                out[o + 1u] = d[i];
            }
            o += 2u;
            i = j;
            continue;
        }
        for (k = i; k < n && k - i < 128u && !(k + 2u < n && d[k] == d[k + 1u] && d[k] == d[k + 2u]); k++)
            ;
        if (out) {
            out[o] = (uint8_t)(k - i - 1u);
            for (j = i; j < k; j++)
                out[o + 1u + (j - i)] = d[j];
        }
        o += 1u + (k - i);
        i = k;
    }
    return o;
}

/* the block's size for this ROM (checked: lsdj_cart_rom_check) and save (LSDJ_CART_SAVE bytes) */
static uint32_t lsdj_cart_need(const uint8_t *rom, uint32_t rom_len, const uint8_t *save)
{
    uint32_t n = rom_len / LSDJ_BANK, need = 2u * n + 4u + lsdj_cart_pack(0, save, LSDJ_CART_SAVE) + LSDJ_CART_TRAILER, i;
    for (i = 0; i < n; i++)
        need += lsdj_cart_trim(rom + i * LSDJ_BANK);
    return need;
}

/* the block into out[0 .. its size): the size, or LSDJ_CART_ROM_* / LSDJ_CART_FULL (more than max bytes) */
static int32_t lsdj_cart_make(uint8_t *out, uint32_t max, const uint8_t *rom, uint32_t rom_len, const uint8_t *save)
{
    uint32_t n = rom_len / LSDJ_BANK, need, i, j, k, at, crc;
    int r = lsdj_cart_rom_check(rom, rom_len);
    uint8_t *t;
    if (r)
        return r;
    need = lsdj_cart_need(rom, rom_len, save);
    if (need > max)
        return LSDJ_CART_FULL;
    for (i = 0; i < n; i++) {
        k = lsdj_cart_trim(rom + i * LSDJ_BANK);
        out[2u * i] = (uint8_t)k;
        out[2u * i + 1u] = (uint8_t)(k >> 8);
    }
    at = 2u * n + 4u;
    k = lsdj_cart_pack(out + at, save, LSDJ_CART_SAVE);
    for (i = 0; i < 4u; i++)
        out[2u * n + i] = (uint8_t)(k >> (8u * i));
    at += k;
    for (i = 0; i < n; i++) {
        k = lsdj_cart_trim(rom + i * LSDJ_BANK);
        for (j = 0; j < k; j++)
            out[at + j] = rom[i * LSDJ_BANK + j];
        at += k;
    }
    crc = lsdj_cart_crc(out, at);
    t = out + at;
    t[0] = 'L'; t[1] = 'S'; t[2] = 'D'; t[3] = 'C';
    t[4] = LSDJ_CART_VERSION; t[5] = 0;
    t[6] = (uint8_t)n; t[7] = 0;
    for (i = 0; i < 4u; i++) {
        t[8u + i] = (uint8_t)(at >> (8u * i));
        t[12u + i] = (uint8_t)(crc >> (8u * i));
    }
    return (int32_t)(at + LSDJ_CART_TRAILER);
}
#endif
