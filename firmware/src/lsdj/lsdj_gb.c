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

/* The programs FMDj runs, one at a time, told apart by the installed ROM's header (bytes 0x134..0x14F: h[0] is
 * 0x134). LSDj: MBC5 + RAM + battery with a 128 KiB save, whatever its title (each version has its own). DMGo!
 * (Drum Machina Go!, Audiowanderer): its title on MBC5 + rumble + RAM + battery with a 32 KiB save; the step keys
 * know v1's memory (measured on the host, 2026-10-17), so only v1, by its global checksum, gets them. */
enum { LSDJ_PROG_NONE = -1, LSDJ_PROG_LSDJ, LSDJ_PROG_DMGO };
#define LSDJ_HDR 28u                          /* header bytes 0x134..0x14F */
#define LSDJ_HB(a) ((a) - 0x134u)             /* a header byte's index in h */

static void lsdj_header(const lsdj_bank_t *t, uint32_t nbanks, uint8_t h[LSDJ_HDR])
{
    uint32_t i;
    for (i = 0; i < LSDJ_HDR; i++)
        h[i] = lsdj_rom_byte(t, nbanks, 0x134u + i);
}

static int lsdj_prog(const uint8_t *h)
{
    static const char DMGO[12] = "DRUMMACHINA";   /* and its NUL */
    uint32_t i;
    if (h[LSDJ_HB(0x147)] == 0x1Bu && h[LSDJ_HB(0x149)] == 0x04u)
        return LSDJ_PROG_LSDJ;
    if (h[LSDJ_HB(0x147)] != 0x1Eu || h[LSDJ_HB(0x149)] != 0x03u)
        return LSDJ_PROG_NONE;
    for (i = 0; i < sizeof DMGO; i++)
        if (h[i] != (uint8_t)DMGO[i])
            return LSDJ_PROG_NONE;
    return LSDJ_PROG_DMGO;
}

static uint32_t lsdj_prog_save(int prog)       /* its save RAM: the size the ROM header says */
{
    return prog == LSDJ_PROG_LSDJ ? 128u * 1024u : prog == LSDJ_PROG_DMGO ? 32u * 1024u : 0;
}

static int lsdj_prog_steps(const uint8_t *h)   /* 1: DMGo v1, whose memory the step keys know */
{
    return lsdj_prog(h) == LSDJ_PROG_DMGO && h[LSDJ_HB(0x14E)] == 0xEEu && h[LSDJ_HB(0x14F)] == 0x3Cu;
}

/* A and B: in LSDj the two F and G keys at the top of the keyboard (fm1_in.notes: bit 0 = F3 .. 26 = G5); in DMGo
 * the black keys D#5 and F#5, as the 16 white keys are its steps (the owner's layout, 2026-10-17) */
#define LSDJ_NOTE_A (1u << 24)                /* F5 */
#define LSDJ_NOTE_B (1u << 26)                /* G5 */
#define DMGO_NOTE_A (1u << 22)                /* D#5 */
#define DMGO_NOTE_B (1u << 25)                /* F#5 */
static const uint32_t LSDJ_STEP_NOTE[16] = {  /* step 1..16: the white keys F3 .. G5, left to right */
    1u << 0, 1u << 2, 1u << 4, 1u << 6, 1u << 7, 1u << 9, 1u << 11, 1u << 12,
    1u << 14, 1u << 16, 1u << 18, 1u << 19, 1u << 21, 1u << 23, 1u << 24, 1u << 26};

/* buttons and note keys held (fm1_in.buttons, .notes) -> the joypad (active low). map: the button bit of each Game
 * Boy key in JOYPAD bit order (A, B, SELECT, START, RIGHT, LEFT, UP, DOWN; 0: no button); lsdj.c fills it from the
 * panel labels. a, b: the note keys of A and B */
static uint8_t lsdj_pad_ab(uint32_t buttons, uint32_t notes, const uint32_t map[8], uint32_t a, uint32_t b)
{
    uint32_t down = 0, i;
    for (i = 0; i < 8u; i++)
        if (buttons & map[i])
            down |= 1u << i;
    if (notes & a)
        down |= JOYPAD_A;
    if (notes & b)
        down |= JOYPAD_B;
    return (uint8_t)~down;
}

static uint8_t lsdj_pad(uint32_t buttons, uint32_t notes, const uint32_t map[8])   /* LSDj's A and B */
{
    return lsdj_pad_ab(buttons, notes, map, LSDJ_NOTE_A, LSDJ_NOTE_B);
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

/* DMGo's step keys (v1: lsdj_prog_steps): white key N toggles step N of the channel page shown. DMGo does it
 * itself: for one frame its cursor is put on the step and B is pressed, the next frame the cursor goes back with B
 * up. The same as the owner moving there and pressing B (the same memory and sound, in any pattern slot; the host,
 * 2026-10-17), without the cursor moving. Its work RAM (offsets from C000): */
#define DMGO_W_PAGE 0x005u                    /* 0..3: CH1..CH4; 4 PATTERNS, 5 SAVE, 6 setup */
#define DMGO_W_MENU 0x006u                    /* 1: the top menu (A) is open */
#define DMGO_W_FIELD 0x01Au                   /* the cursor: its field (12: the SEQ row) */
#define DMGO_W_COL 0x01Bu                     /* ... its column (0..15), kept twice */
#define DMGO_W_COL2 0xB1Fu
#define DMGO_W_STARTED 0x02Fu                 /* 0 on the title screen */
#define DMGO_W_PICK 0xA34u                    /* 1: PICK A PAT is open */
#define DMGO_FIELD_SEQ 12u
typedef struct {
    uint8_t q[16], n;                         /* steps waiting (0..15), the oldest first */
    uint8_t busy, keep[3];                    /* the step's frame ran: the cursor (field, column, column) to put back */
    uint8_t out;                              /* the keys DMGo got last frame */
    uint8_t fresh;                            /* the Game Boy restarted: nothing until DMGo has shown its title */
    uint32_t held;                            /* the white keys down: a press is a key that was up */
} lsdj_steps_t;

/* the Game Boy starts again (PLAY, `ble go reset`): what waits is dropped, and until DMGo has been on its title
 * screen its work RAM is the last run's (a reset does not clear it), so no step is made. Keys held stay held */
static void lsdj_steps_restart(lsdj_steps_t *s)
{
    s->n = 0;
    s->busy = 0;
    s->fresh = 1;
}

static void lsdj_steps_keys(lsdj_steps_t *s, uint32_t notes)   /* the note keys held now: new presses wait */
{
    uint32_t i, down = notes & ~s->held;
    s->held = notes;
    for (i = 0; i < 16u; i++)
        if ((down & LSDJ_STEP_NOTE[i]) && s->n < sizeof s->q)
            s->q[s->n++] = (uint8_t)i;
}

/* before a Game Boy frame: pad, the owner's keys (active low) -> the keys for it. A step waits while any key is down
 * or was down last frame: DMGo takes B as a press, B with a direction or A is another edit, and a key held through
 * the step's frame would come back as a new press. On any page but a channel page with nothing over it, it is
 * dropped */
static uint8_t lsdj_steps_frame(lsdj_steps_t *s, uint8_t *wram, uint8_t pad)
{
    uint32_t i, step;
    if (s->busy) {
        wram[DMGO_W_FIELD] = s->keep[0];
        wram[DMGO_W_COL] = s->keep[1];
        wram[DMGO_W_COL2] = s->keep[2];
        s->busy = 0;
        return s->out = (uint8_t)(pad | JOYPAD_B);
    }
    if (s->fresh) {
        if (!wram[DMGO_W_STARTED])
            s->fresh = 0;
        s->n = 0;
        return s->out = pad;
    }
    if (!s->n || s->out != 0xFFu || pad != 0xFFu)
        return s->out = pad;
    step = s->q[0];
    for (i = 1; i < s->n; i++)
        s->q[i - 1u] = s->q[i];
    s->n--;
    if (!wram[DMGO_W_STARTED] || wram[DMGO_W_PAGE] > 3u || wram[DMGO_W_MENU] || wram[DMGO_W_PICK])
        return s->out = pad;
    s->keep[0] = wram[DMGO_W_FIELD];
    s->keep[1] = wram[DMGO_W_COL];
    s->keep[2] = wram[DMGO_W_COL2];
    wram[DMGO_W_FIELD] = DMGO_FIELD_SEQ;
    wram[DMGO_W_COL] = wram[DMGO_W_COL2] = (uint8_t)step;
    s->busy = 1;
    return s->out = (uint8_t)~JOYPAD_B;
}

/* The GLO menu (the owner's, 2026-10-22): the screen's colours, the status bar, the emulator's four options (once
 * the FX, LFO, EDIT, ENV buttons). Kept in flash as two bytes (lsdj_store.c, the layout record); 0, 0 is every
 * default: DMG green, no status bar, every option on. */
enum { LSDJ_THEME_DMG, LSDJ_THEME_POCKET, LSDJ_THEME_LIGHT, LSDJ_THEME_SGB, LSDJ_THEME_DARK, LSDJ_NTHEME };
static const char LSDJ_THEME_NAME[LSDJ_NTHEME][8] = {"DMG", "POCKET", "LIGHT", "SGB", "DARK"};
static const uint8_t LSDJ_THEME_RGB[LSDJ_NTHEME][4][3] = {   /* shade 0 (the Game Boy's lightest) .. 3 */
    {{0x9B, 0xBC, 0x0F}, {0x8B, 0xAC, 0x0F}, {0x30, 0x62, 0x30}, {0x0F, 0x38, 0x0F}},   /* the first Game Boy */
    {{232, 232, 224}, {160, 160, 152}, {88, 88, 84}, {16, 16, 16}},                     /* Pocket: the grey of before */
    {{0x80, 0xE8, 0xD8}, {0x40, 0xB8, 0xA8}, {0x18, 0x70, 0x68}, {0x00, 0x30, 0x30}},   /* Light: the backlit one */
    {{0xF8, 0xE8, 0xC8}, {0xD8, 0x90, 0x48}, {0xA8, 0x28, 0x20}, {0x30, 0x18, 0x50}},   /* Super Game Boy, 1-A */
    {{16, 16, 20}, {72, 72, 80}, {150, 150, 160}, {232, 232, 224}}};                    /* Dark: light ink on black */

typedef struct {
    uint8_t theme;                            /* LSDJ_THEME_* */
    uint8_t bar;                              /* the status bar under the picture */
    uint8_t off;                              /* the emulator's options switched off: lsdj_emu.c emu_mode bits */
} lsdj_set_t;
static void lsdj_set_pack(const lsdj_set_t *s, uint8_t b[2])
{
    b[0] = s->theme;
    b[1] = (uint8_t)((s->bar ? 0x10u : 0u) | (s->off & 0x0Fu));
}
static void lsdj_set_unpack(lsdj_set_t *s, const uint8_t b[2])
{
    s->theme = b[0] < LSDJ_NTHEME ? b[0] : LSDJ_THEME_DMG;
    s->bar = (b[1] & 0x10u) != 0;
    s->off = b[1] & 0x0Fu;
}

/* the lines, top to bottom; an emulator option's line is its emu_mode bit (EMU_FRAME_SKIP 2, EMU_FAST_ROM 1,
 * EMU_WAIT_SKIP 4, EMU_SND_FINE 8) */
enum { LSDJ_MI_SCREEN, LSDJ_MI_BAR, LSDJ_MI_DRAW, LSDJ_MI_ROM, LSDJ_MI_WAIT, LSDJ_MI_SOUND, LSDJ_NMI };
static const uint8_t LSDJ_MI_BIT[LSDJ_NMI] = {0, 0, 2, 1, 4, 8};
typedef struct {
    uint8_t open, item;
    lsdj_set_t set;
} lsdj_menu_t;

/* a key pressed in the menu (one JOYPAD bit): UP / DOWN the line, LEFT / RIGHT / A its value. 1: a value changed */
static int lsdj_menu_key(lsdj_menu_t *m, uint8_t key)
{
    int step = key == JOYPAD_LEFT ? -1 : key == JOYPAD_RIGHT || key == JOYPAD_A ? 1 : 0;
    if (key == JOYPAD_UP || key == JOYPAD_DOWN) {
        m->item = (uint8_t)((m->item + (key == JOYPAD_UP ? LSDJ_NMI - 1u : 1u)) % LSDJ_NMI);
        return 0;
    }
    if (!step)
        return 0;
    if (m->item == LSDJ_MI_SCREEN)
        m->set.theme = (uint8_t)((m->set.theme + LSDJ_NTHEME + step) % LSDJ_NTHEME);
    else if (m->item == LSDJ_MI_BAR)
        m->set.bar ^= 1u;
    else
        m->set.off ^= LSDJ_MI_BIT[m->item];
    return 1;
}

static void lsdj_menu_line(const lsdj_menu_t *m, uint32_t item, char *label, char *value)   /* (>= 16 bytes each) */
{
    static const char LABEL[LSDJ_NMI][12] = {"SCREEN", "STATUS BAR", "DRAW", "ROM READ", "WAIT LOOP", "SOUND"};
    static const char ON[LSDJ_NMI][12] = {"", "ON", "EVERY 2ND", "FAST", "SKIP", "IN FRAME"};
    static const char OFF[LSDJ_NMI][12] = {"", "OFF", "EVERY", "PLAIN", "RUN", "PER FRAME"};
    const char *v;
    uint32_t i;
    if (item == LSDJ_MI_SCREEN)
        v = LSDJ_THEME_NAME[m->set.theme];
    else if (item == LSDJ_MI_BAR)
        v = m->set.bar ? ON[item] : OFF[item];
    else
        v = m->set.off & LSDJ_MI_BIT[item] ? OFF[item] : ON[item];
    for (i = 0; LABEL[item][i]; i++)
        label[i] = LABEL[item][i];
    label[i] = 0;
    for (i = 0; v[i]; i++)
        value[i] = v[i];
    value[i] = 0;
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
