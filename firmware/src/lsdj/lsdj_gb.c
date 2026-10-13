/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj unit's pure parts (no core, no hardware; host-tested by tests/lsdj/lsdj_gb_test.c):
 *   the ROM as a bank table (each 16 KiB bank stored without its trailing 0xFF bytes),
 *   the FM-1 buttons and two note keys -> the Game Boy joypad, knob steps -> short presses, the frame buffer (2-bit
 *   shades) and the dirty span of a line pair,
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

/* A and B are the two F and G keys at the top of the keyboard (fm1_in.notes: bit 0 = F3 .. 26 = G5) */
#define LSDJ_NOTE_A (1u << 24)                /* F5 */
#define LSDJ_NOTE_B (1u << 26)                /* G5 */

/* buttons and note keys held (fm1_in.buttons, .notes) -> the joypad (active low). map: the button bit of each Game
 * Boy key in JOYPAD bit order (A, B, SELECT, START, RIGHT, LEFT, UP, DOWN; 0: no button); lsdj.c fills it from the
 * panel labels */
static uint8_t lsdj_pad(uint32_t buttons, uint32_t notes, const uint32_t map[8])
{
    uint32_t down = 0, i;
    for (i = 0; i < 8u; i++)
        if (buttons & map[i])
            down |= 1u << i;
    if (notes & LSDJ_NOTE_A)
        down |= JOYPAD_A;
    if (notes & LSDJ_NOTE_B)
        down |= JOYPAD_B;
    return (uint8_t)~down;
}

/* Knob steps -> short presses, one Game Boy frame at a time. LSDj reads its keys once a frame; measured on 9.4.2
 * (the host harness, 2026-10-12): a direction down one frame and up one frame is exactly one press, and SELECT +
 * direction moves one screen only if SELECT is down before the direction: two frames before, because the
 * PROJECT screen does not take it after one and could not be left (the owner's FM-1 and the host, 2026-10-14).
 * Axes: + = right / down; the NAV ones press SELECT with the direction (LSDj's screen map). */
enum { LSDJ_AX_LR, LSDJ_AX_UD, LSDJ_AX_NAV_LR, LSDJ_AX_NAV_UD, LSDJ_NAX };
#define LSDJ_KNOB_MAX 16                      /* steps kept waiting per axis: a spun knob does not run on */
typedef struct {
    int8_t want[LSDJ_NAX];                    /* steps not yet sent */
    uint8_t seq[5], n, i;                     /* the press being sent: keys down (JOYPAD bits) per frame */
    uint8_t next;                             /* the axis looked at first: the knobs take turns */
} lsdj_knobs_t;

static void lsdj_knobs_add(lsdj_knobs_t *k, uint32_t axis, int32_t steps)   /* a turn back cancels what waits */
{
    int32_t v = k->want[axis] + steps;
    k->want[axis] = (int8_t)(v > LSDJ_KNOB_MAX ? LSDJ_KNOB_MAX : v < -LSDJ_KNOB_MAX ? -LSDJ_KNOB_MAX : v);
}

static uint8_t lsdj_knobs_frame(lsdj_knobs_t *k)       /* the keys down (1 = down) in the next Game Boy frame */
{
    static const uint8_t KEY[2][2] = {{JOYPAD_LEFT, JOYPAD_RIGHT}, {JOYPAD_UP, JOYPAD_DOWN}};
    uint32_t t, a;
    if (k->i < k->n)
        return k->seq[k->i++];
    for (t = 0; t < LSDJ_NAX; t++) {
        uint8_t key;
        a = (k->next + t) % LSDJ_NAX;
        if (!k->want[a])
            continue;
        key = KEY[a & 1u][k->want[a] > 0];
        k->want[a] = (int8_t)(k->want[a] + (k->want[a] > 0 ? -1 : 1));
        k->next = (uint8_t)((a + 1u) % LSDJ_NAX);
        if (a >= LSDJ_AX_NAV_LR) {
            k->seq[0] = JOYPAD_SELECT;
            k->seq[1] = JOYPAD_SELECT;
            k->seq[2] = JOYPAD_SELECT | key;
            k->seq[3] = JOYPAD_SELECT;
            k->seq[4] = 0;
            k->n = 5;
        } else {
            k->seq[0] = key;
            k->seq[1] = 0;
            k->n = 2;
        }
        k->i = 1;
        return k->seq[0];
    }
    return 0;
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

static int lsdj_line(uint8_t *row, const uint8_t *px)      /* Peanut-GB's line: shade in bits 0..1; 1 = changed */
{
    uint32_t x, d = 0;
    for (x = 0; x < LSDJ_W; x++) {
        uint8_t v = px[x] & 3u;
        d |= row[x] ^ v;
        row[x] = v;
    }
    return d != 0;
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

/* speed % over a window: frames run against the Game Boy's frame rate (70224 / 4194304 s = 16742.7 us a frame),
 * rounded; 32-bit only (frames <= 25000) */
static uint32_t lsdj_speed(uint32_t frames, uint32_t ms)
{
    if (!ms)
        return 0;
    return (frames * 167427u + ms * 50u) / (ms * 100u);
}

/* the MASTER pot -> gain Q12 (4096 = 1.0), as Felucca's main loop: an IIR on the 10-bit reading (knob: 16x, state),
 * then squared (an audio taper). adc < 0 (an ADC timeout) keeps the last gain */
static uint32_t lsdj_master(int32_t *knob, int32_t adc)
{
    uint32_t k10;
    if (adc >= 0)
        *knob += (adc * 16 - *knob) / 8;
    k10 = (uint32_t)(*knob / 16);
    return (k10 * k10) >> 8;                  /* 0 .. 4088 */
}

static void lsdj_gain(int32_t *out, uint32_t n, uint32_t q12)    /* stereo Q15 frames, in place */
{
    uint32_t i;
    for (i = 0; i < 2u * n; i++)
        out[i] = (out[i] * (int32_t)q12) >> 12;
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

static uint32_t lsdj_crc32_add(uint32_t c, uint8_t b)  /* zlib CRC-32, 4 bits a step: start ~0, the CRC is ~c */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    c ^= b;
    c = (c >> 4) ^ T[c & 15u];
    return (c >> 4) ^ T[c & 15u];
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
