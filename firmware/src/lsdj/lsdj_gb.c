/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj unit's pure parts (no core, no hardware; host-tested by tests/lsdj/lsdj_gb_test.c):
 *   the ROM as a bank table (each 16 KiB bank stored without its trailing 0xFF bytes),
 *   the FM-1 keys -> the Game Boy joypad, the frame buffer (2-bit shades) and the dirty span of a line pair,
 *   the 1.5x scaler (2 x 2 Game Boy pixels -> 3 x 3 screen pixels), the audio ring (main loop -> audio ISR),
 *   the PackBits unpacker (the start-up save). */
#include <stdint.h>

#define LSDJ_W 160u
#define LSDJ_H 144u
#define LSDJ_BANK 0x4000u
#define LSDJ_SX(x) ((x) * 3u / 2u)            /* Game Boy x (even) -> screen x */

#ifndef JOYPAD_A                              /* Peanut-GB's joypad bits (active low in gb.direct.joypad) */
#define JOYPAD_A 0x01
#define JOYPAD_B 0x02
#define JOYPAD_SELECT 0x04
#define JOYPAD_START 0x08
#define JOYPAD_RIGHT 0x10
#define JOYPAD_LEFT 0x20
#define JOYPAD_UP 0x40
#define JOYPAD_DOWN 0x80
#endif

typedef struct {
    const uint8_t *p;
    uint16_t n;                               /* bytes stored; the rest of the bank reads 0xFF */
} lsdj_bank_t;

static inline uint8_t lsdj_rom_byte(const lsdj_bank_t *t, uint32_t nbanks, uint32_t off)
{
    uint32_t b = off / LSDJ_BANK, i = off % LSDJ_BANK;
    if (b >= nbanks || i >= t[b].n)
        return 0xFF;
    return t[b].p[i];
}

/* note keys (fm1_in.notes, bit 0 = F3 .. 26 = G5): the D-pad on the lowest keys as an inverted T,
 * B and A on the two highest white keys; START = PLAY, SELECT = REC. OCT- / OCT+ stay the core's (UBOOT). */
#define LSDJ_KEY_LEFT 0u      /* F3 */
#define LSDJ_KEY_DOWN 2u      /* G3 */
#define LSDJ_KEY_UP 3u        /* G#3 */
#define LSDJ_KEY_RIGHT 4u     /* A3 */
#define LSDJ_KEY_B 24u        /* F5 */
#define LSDJ_KEY_A 26u        /* G5 */

static uint8_t lsdj_pad(uint32_t buttons, uint32_t notes, uint32_t play_bit, uint32_t rec_bit)
{
    uint32_t down = 0;
    if (buttons & play_bit)
        down |= JOYPAD_START;
    if (buttons & rec_bit)
        down |= JOYPAD_SELECT;
    if (notes & (1u << LSDJ_KEY_LEFT))
        down |= JOYPAD_LEFT;
    if (notes & (1u << LSDJ_KEY_DOWN))
        down |= JOYPAD_DOWN;
    if (notes & (1u << LSDJ_KEY_UP))
        down |= JOYPAD_UP;
    if (notes & (1u << LSDJ_KEY_RIGHT))
        down |= JOYPAD_RIGHT;
    if (notes & (1u << LSDJ_KEY_B))
        down |= JOYPAD_B;
    if (notes & (1u << LSDJ_KEY_A))
        down |= JOYPAD_A;
    return (uint8_t)~down;
}

/* the Game Boy keys held in pad (active low) as text, e.g. "START SEL R"; "-" if none (b: >= 22 bytes) */
static void lsdj_pad_text(char *b, uint8_t pad)
{
    static const struct { uint8_t bit; char name[6]; } K[8] = {
        {JOYPAD_START, "START"}, {JOYPAD_SELECT, "SEL"}, {JOYPAD_A, "A"}, {JOYPAD_B, "B"},
        {JOYPAD_LEFT, "L"}, {JOYPAD_RIGHT, "R"}, {JOYPAD_UP, "U"}, {JOYPAD_DOWN, "D"}};
    uint32_t i;
    const char *s;
    char *o = b;
    for (i = 0; i < 8u; i++) {
        if (pad & K[i].bit)
            continue;
        if (o != b)
            *o++ = ' ';
        for (s = K[i].name; *s; s++)
            *o++ = *s;
    }
    if (o == b)
        *o++ = '-';
    *o = 0;
}

static void lsdj_line(uint8_t *row, const uint8_t *px)     /* Peanut-GB's line: shade in bits 0..1 */
{
    uint32_t x;
    for (x = 0; x < LSDJ_W; x++)
        row[x] = px[x] & 3u;
}

/* the line pair at fb (rows fb, fb + LSDJ_W) against what the screen shows: changed -> 1 and [x0, x1), even */
static int lsdj_span(const uint8_t *fb, const uint8_t *shown, uint32_t *x0, uint32_t *x1)
{
    uint32_t a = LSDJ_W, b = 0, r, x;
    for (r = 0; r < 2u; r++) {
        const uint8_t *f = fb + r * LSDJ_W, *s = shown + r * LSDJ_W;
        for (x = 0; x < LSDJ_W; x++)
            if (f[x] != s[x]) {
                if (x < a)
                    a = x;
                break;
            }
        for (x = LSDJ_W; x > 0u; x--)
            if (f[x - 1u] != s[x - 1u]) {
                if (x > b)
                    b = x;
                break;
            }
    }
    if (a >= b)
        return 0;
    *x0 = a & ~1u;
    *x1 = (b + 1u) & ~1u;
    return 1;
}

/* 3 screen rows of LSDJ_SX(x1 - x0) pixels from the line pair at fb: rows 0, 1 = line 0, row 2 = line 1;
 * each pixel pair a b -> a a b. pal: the 4 shades as the panel wants them (RGB565, byte-swapped) */
static void lsdj_scale(uint16_t *out, const uint8_t *fb, uint32_t x0, uint32_t x1, const uint16_t pal[4])
{
    uint32_t w = LSDJ_SX(x1 - x0), x, o;
    for (o = 0, x = x0; x < x1; x += 2u) {
        uint16_t a = pal[fb[x]], b = pal[fb[x + 1u]];
        out[o] = a;
        out[o + 1u] = a;
        out[o + 2u] = b;
        o += 3u;
    }
    for (x = 0; x < w; x++)
        out[w + x] = out[x];
    fb += LSDJ_W;
    for (o = 2u * w, x = x0; x < x1; x += 2u) {
        uint16_t a = pal[fb[x]], b = pal[fb[x + 1u]];
        out[o] = a;
        out[o + 1u] = a;
        out[o + 2u] = b;
        o += 3u;
    }
}

/* stereo int16 frames, one writer (main loop) and one reader (audio ISR); size a power of two, one slot spare */
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif
typedef struct {
    int16_t *buf;
    uint32_t size;
    volatile uint32_t wr, rd;
    uint32_t under;                           /* frames the reader found missing */
    int16_t last_l, last_r;
} lsdj_ring_t;

static void lsdj_ring_init(lsdj_ring_t *r, int16_t *buf, uint32_t size)
{
    r->buf = buf;
    r->size = size;
    r->wr = r->rd = 0;
    r->under = 0;
    r->last_l = r->last_r = 0;
}

static uint32_t lsdj_ring_fill(const lsdj_ring_t *r) { return (r->wr - r->rd) & (r->size - 1u); }
static uint32_t lsdj_ring_free(const lsdj_ring_t *r) { return r->size - 1u - lsdj_ring_fill(r); }

static void lsdj_ring_push(lsdj_ring_t *r, const int16_t *st, uint32_t n)   /* n <= lsdj_ring_free */
{
    uint32_t w = r->wr, i;
    for (i = 0; i < n; i++) {
        r->buf[2u * w] = st[2u * i];
        r->buf[2u * w + 1u] = st[2u * i + 1u];
        w = (w + 1u) & (r->size - 1u);
    }
    RING_PUBLISH();
    r->wr = w;
}

static void lsdj_ring_pull(lsdj_ring_t *r, int32_t *out, uint32_t n)        /* Q15 stereo */
{
    uint32_t rd = r->rd, wr = r->wr, i;
    for (i = 0; i < n; i++) {
        if (rd != wr) {
            r->last_l = r->buf[2u * rd];
            r->last_r = r->buf[2u * rd + 1u];
            rd = (rd + 1u) & (r->size - 1u);
        } else {
            r->under++;
        }
        out[2u * i] = r->last_l;
        out[2u * i + 1u] = r->last_r;
    }
    RING_PUBLISH();
    r->rd = rd;
}

/* PackBits: c < 128 -> c + 1 literal bytes follow; c >= 128 -> (c & 127) + 1 copies of the next byte.
 * 0 = exactly n bytes written */
static int lsdj_unpack(uint8_t *dst, uint32_t n, const uint8_t *src, uint32_t len)
{
    uint32_t o = 0, i = 0;
    while (i < len) {
        uint32_t c = src[i++], k = (c & 127u) + 1u;
        if (o + k > n)
            return -1;
        if (c < 128u) {
            if (i + k > len)
                return -1;
            while (k--)
                dst[o++] = src[i++];
        } else {
            if (i >= len)
                return -1;
            while (k--)
                dst[o++] = src[i];
            i++;
        }
    }
    return o == n ? 0 : -1;
}
