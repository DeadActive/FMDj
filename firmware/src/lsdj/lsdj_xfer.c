/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj unit's transfer commands: what follows `ble` on a console line -> one reply line. Pure (no core, no
 * hardware; host-tested by tests/lsdj/lsdj_xfer_test.c). Include lsdj_gb.c first.
 *   info               ok v1 started S held H banks NN
 *   hold               ok | err not started     the Game Boy stops between two frames (lsdj.c)
 *   go [reset]         ok                       it runs again; reset: LSDj starts again first
 *   r ADDR LEN         ok HEX CRC16             save RAM, LEN 1..100 (256 bytes); needs hold
 *   w ADDR HEX CRC16   ok                       save RAM, 1..20 bytes (the console line is 64 characters);
 *                                               needs hold; an error writes nothing
 *   b BANK OFF LEN     ok HEX CRC16             ROM bank bytes (past the stored ones: FF), LEN 1..100
 *   bc BANK            ok CRC32                 of the whole 16 KiB bank
 * Numbers are hex, 1..8 digits, no prefix. CRC16: XMODEM (0x1021, start 0) of the data bytes, 4 digits.
 * Errors: err ? (not a command), err hold, err range, err hex, err crc. */
#define LSDJ_XFER_VERSION 1u
#define LSDJ_XFER_RD 256u                     /* bytes in a read reply */
#define LSDJ_XFER_WR 20u                      /* bytes in a write line: "ble w 1FFEC " + 40 + " CRC " <= 63 */
#define LSDJ_XFER_OUT (3u + 2u * LSDJ_XFER_RD + 1u + 4u + 1u)   /* "ok " HEX " " CRC and the NUL */
#define LSDJ_XFER_IDLE_MS 5000u               /* held and no command for this long: the host went away */

typedef struct {
    uint8_t *sram;                            /* the save RAM */
    uint32_t size;
    const lsdj_bank_t *rom;
    uint32_t nbanks;
    uint8_t started, held, reset_req;         /* started: PLAY ran LSDj (lsdj.c sets it, and carries out reset_req) */
    uint32_t last_ms;                         /* the last command while held */
} lsdj_xfer_t;

static uint16_t lsdj_crc16(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0, i;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8u; i++)
            c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
    }
    return (uint16_t)c;
}

static uint32_t lsdj_crc32_add(uint32_t c, uint8_t b)  /* zlib CRC-32, 4 bits a step: start ~0, the CRC is ~c */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    c ^= b;
    c = (c >> 4) ^ T[c & 15u];
    return (c >> 4) ^ T[c & 15u];
}

static int xf_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int xf_word(const char **p, const char *w)       /* a whole word: 1, and *p after it */
{
    const char *s = *p;
    while (*s == ' ')
        s++;
    while (*w && *s == *w)
        s++, w++;
    if (*w || (*s && *s != ' '))
        return 0;
    *p = s;
    return 1;
}

static int xf_num(const char **p, uint32_t *v)          /* 1..8 hex digits as a whole word: 1 */
{
    const char *s = *p;
    uint32_t n = 0, x = 0;
    int d;
    while (*s == ' ')
        s++;
    while (n < 9u && (d = xf_digit(*s)) >= 0) {
        x = x << 4 | (uint32_t)d;
        s++;
        n++;
    }
    if (!n || n > 8u || (*s && *s != ' '))
        return 0;
    *v = x;
    *p = s;
    return 1;
}

static int xf_end(const char *p)
{
    while (*p == ' ')
        p++;
    return !*p;
}

static char *xf_puts(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    *o = 0;
    return o;
}

static char *xf_hex(char *o, uint32_t v, uint32_t digits)
{
    while (digits--)
        *o++ = "0123456789ABCDEF"[(v >> (4u * digits)) & 15u];
    *o = 0;
    return o;
}

static void xf_data(char *out, const uint8_t *d, uint32_t n)   /* "ok HEX CRC" */
{
    uint32_t i;
    char *o = xf_puts(out, "ok ");
    for (i = 0; i < n; i++)
        o = xf_hex(o, d[i], 2);
    *o++ = ' ';
    xf_hex(o, lsdj_crc16(d, n), 4);
}

/* the data word of a write: -1 not hex pairs, -2 more than LSDJ_XFER_WR bytes, else the byte count (0: none) */
static int xf_bytes(const char **p, uint8_t *d)
{
    const char *s = *p;
    uint32_t n = 0;
    int hi, lo;
    while (*s == ' ')
        s++;
    while (*s && *s != ' ') {
        if ((hi = xf_digit(s[0])) < 0 || (lo = xf_digit(s[1])) < 0)
            return -1;
        if (n == LSDJ_XFER_WR)
            return -2;
        d[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    *p = s;
    return (int)n;
}

static void lsdj_xfer_cmd(lsdj_xfer_t *x, const char *p, uint32_t now_ms, char *out)
{
    uint8_t d[LSDJ_XFER_RD];
    uint32_t a, n, b, i, crc;
    int k;
    if (x->held)
        x->last_ms = now_ms;
    if (xf_word(&p, "info")) {
        char *o;
        if (!xf_end(p))
            goto unknown;
        o = xf_puts(out, "ok v");
        o = xf_hex(o, LSDJ_XFER_VERSION, 1);
        o = xf_puts(o, " started ");
        o = xf_hex(o, x->started, 1);
        o = xf_puts(o, " held ");
        o = xf_hex(o, x->held, 1);
        o = xf_puts(o, " banks ");
        xf_hex(o, x->nbanks, 2);
    } else if (xf_word(&p, "hold")) {
        if (!xf_end(p))
            goto unknown;
        if (!x->started) {
            xf_puts(out, "err not started");
            return;
        }
        x->held = 1;
        x->last_ms = now_ms;
        xf_puts(out, "ok");
    } else if (xf_word(&p, "go")) {
        int reset = xf_word(&p, "reset");
        if (!xf_end(p))
            goto unknown;
        x->held = 0;
        if (reset && x->started)
            x->reset_req = 1;
        xf_puts(out, "ok");
    } else if (xf_word(&p, "r")) {
        if (!xf_num(&p, &a) || !xf_num(&p, &n) || !xf_end(p))
            goto unknown;
        if (!x->held) {
            xf_puts(out, "err hold");
            return;
        }
        if (!n || n > LSDJ_XFER_RD || a > x->size || n > x->size - a) {
            xf_puts(out, "err range");
            return;
        }
        xf_data(out, x->sram + a, n);
    } else if (xf_word(&p, "w")) {
        if (!xf_num(&p, &a))
            goto unknown;
        k = xf_bytes(&p, d);
        if (k == -1) {
            xf_puts(out, "err hex");
            return;
        }
        if (k == -2) {
            xf_puts(out, "err range");
            return;
        }
        if (!k || !xf_num(&p, &crc) || !xf_end(p))
            goto unknown;
        n = (uint32_t)k;
        if (!x->held) {
            xf_puts(out, "err hold");
            return;
        }
        if (a > x->size || n > x->size - a) {
            xf_puts(out, "err range");
            return;
        }
        if (crc != lsdj_crc16(d, n)) {
            xf_puts(out, "err crc");
            return;
        }
        for (i = 0; i < n; i++)
            x->sram[a + i] = d[i];
        xf_puts(out, "ok");
    } else if (xf_word(&p, "b")) {
        if (!xf_num(&p, &b) || !xf_num(&p, &a) || !xf_num(&p, &n) || !xf_end(p))
            goto unknown;
        if (b >= x->nbanks || !n || n > LSDJ_XFER_RD || a > LSDJ_BANK || n > LSDJ_BANK - a) {
            xf_puts(out, "err range");
            return;
        }
        for (i = 0; i < n; i++)
            d[i] = lsdj_rom_byte(x->rom, x->nbanks, b * LSDJ_BANK + a + i);
        xf_data(out, d, n);
    } else if (xf_word(&p, "bc")) {
        if (!xf_num(&p, &b) || !xf_end(p))
            goto unknown;
        if (b >= x->nbanks) {
            xf_puts(out, "err range");
            return;
        }
        crc = 0xFFFFFFFFu;
        for (i = 0; i < LSDJ_BANK; i++)
            crc = lsdj_crc32_add(crc, lsdj_rom_byte(x->rom, x->nbanks, b * LSDJ_BANK + i));
        xf_hex(xf_puts(out, "ok "), ~crc, 8);
    } else {
unknown:
        xf_puts(out, "err ?");
    }
}

static void lsdj_xfer_tick(lsdj_xfer_t *x, uint32_t now_ms)   /* held and the host went away: run again */
{
    if (x->held && now_ms - x->last_ms >= LSDJ_XFER_IDLE_MS)
        x->held = 0;
}
