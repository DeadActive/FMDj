/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj unit (FELUCCA_LSDJ=1): its own object, linked in place of the BLE test unit. The core's unit is
 * compiled exactly as in the BLE test build (FELUCCA_BLE=1: its main-loop hooks ble_key / ble_service / ble_draw,
 * the console's `ble`), so the update path is the one proven on the device (gate 2b,
 * tools/check_update_path.py). The core calls the hooks below; this unit calls the core only through ble_core.h.
 *
 * Nothing runs at boot: PLAY starts the Game Boy (ble_key). From then on, each main-loop pass runs one Game Boy
 * frame when the audio ring has room for its sound (the audio ISR plays the ring: lsdj_audio), then sends a few
 * changed line pairs to the screen (1.5x: 240 x 216). The emulator is reached only through function pointers set
 * by lsdj_start (gate 4, tools/check_ble_boot.py --lsdj, walks the direct calls from boot).
 *
 * build/lsdj/lsdj_rom.h (tools/lsdj_rom.py): the owner's own LSDj ROM and the start-up save. A private build only:
 * LSDj's licence forbids distributing it, so that header, and every firmware built with it, stays in build/
 * (git-ignored) and is never published. */
#include "ble/ble_core.h"
#ifndef LSDJ_POOL
#define LSDJ_POOL __attribute__((section(".pool")))   /* big zero-initialised buffers (app.ld POOL) */
#endif
#include "lsdj/lsdj_gb.c"
#include "lsdj/lsdj_emu.c"
#include "lsdj_rom.h"

#define RING_N 4096u                          /* stereo frames: 93 ms, 5.5 Game Boy frames */
#define Y0 4u                                 /* the picture: rows 4 .. 219; the status line below */
#define GROUPS (LSDJ_H / 2u)                  /* line pairs: 3 screen rows each */
#define PUSH_MAX 8u                           /* line pairs merged into one transfer (24 rows, <= 11.5 KB) */
#define STATUS_Y 223u

static uint8_t sram[LSDJ_SRAM] LSDJ_POOL;
static uint8_t fb[LSDJ_H][LSDJ_W] LSDJ_POOL, shown[LSDJ_H][LSDJ_W] LSDJ_POOL;
static uint8_t dirty[LSDJ_H];                 /* rows changed since they were sent (set by emu_line) */
static int16_t ring_buf[2u * RING_N] LSDJ_POOL;
static uint16_t pushbuf[2][PUSH_MAX * 3u * 240u] LSDJ_POOL;   /* DMA sources: one filled while the other goes out */
static lsdj_ring_t ring;
static void (*volatile run)(void);            /* lsdj_step once started */
static void (*volatile draw)(void);           /* lsdj_push once started */
static uint32_t group, buf_i, splash_done;
static uint32_t pad_map[8];                   /* button bit per Game Boy key (JOYPAD order), from KEYS */

/* the Game Boy keys on the panel's buttons (labels), in JOYPAD bit order: A, B, SELECT, START, RIGHT, LEFT, UP, DOWN.
 * OCT- / OCT+ stay the core's (held 5 s together: UBOOT) */
static const uint8_t KEYS[8] = {CORE_BTN_GLO, CORE_BTN_EDIT, CORE_BTN_PLAY, CORE_BTN_REC,
                                CORE_BTN_ARP, CORE_BTN_HOME, CORE_BTN_SCL, CORE_BTN_SAVE};

/* RGB565, byte-swapped as the panel takes them: shade 0 (light) .. 3 (dark) */
#define SW(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
#define RGB(r, g, b) SW((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
static const uint16_t PAL[4] = {RGB(232, 232, 224), RGB(160, 160, 152), RGB(88, 88, 84), RGB(16, 16, 16)};

static struct {
    uint32_t frames, waits, frame_max_us, frame_us_sum, pushes, start_ms, push_ticks, push_calls;
    uint32_t sec_ms, sec_frames, sec_under, last_fps, last_under, sec_us, last_load;
    uint32_t win_ms, win_frames, last_speed;  /* SPD: over 4 s windows (whole frames in 1 s jitter by 1.7 %) */
    uint32_t buttons, notes;                  /* as read for the last frame (console `ble`) */
    int32_t pot, knob;                        /* the MASTER pot: last reading, its filter state */
    volatile uint32_t master_q12;             /* its gain, applied by the audio ISR */
    uint8_t pad, pad_shown;                   /* the Game Boy keys of the last frame; those on the status line */
    int error;
} st;

/* ---- the audio ISR's source (core_audio.c snd_block, FELUCCA_LSDJ): Q15 stereo ---- */
void lsdj_audio(int32_t *out, uint32_t n)
{
    uint32_t i;
    if (!run) {
        for (i = 0; i < 2u * n; i++)
            out[i] = 0;
        return;
    }
    lsdj_ring_pull(&ring, out, n);
    lsdj_gain(out, n, st.master_q12);         /* the MASTER pot */
}

/* ---- text ---- */
static char *put_s(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}
static char *put_u(char *b, uint32_t v)
{
    char t[12];
    uint32_t n = 0;
    do
        t[n++] = (char)('0' + v % 10u);
    while (v /= 10u);
    while (n)
        *b++ = t[--n];
    *b = 0;
    return b;
}

static char *put_hex(char *b, uint32_t v)
{
    uint32_t i = 28u;
    while (i && !(v >> i))
        i -= 4u;
    for (;; i -= 4u) {
        *b++ = "0123456789ABCDEF"[(v >> i) & 15u];
        if (!i)
            break;
    }
    *b = 0;
    return b;
}

/* left: SPD = Game Boy frames per second against 60; LOAD = the emulator's time per frame against a Game Boy frame
 * (16.74 ms): over 100 % it cannot keep up, under 100 % it waits for the audio */
static void status_line(void)
{
    char b[48], *p = b;
    p = put_s(p, "SPD ");
    p = put_u(p, st.last_speed);
    p = put_s(p, "% LOAD ");
    p = put_u(p, st.last_load);
    put_s(p, "%");
    core_text(0, (int)STATUS_Y, 112, CORE_FONT_S, b, CORE_MID, CORE_BG, 0);
}

/* right: the Game Boy keys held, or with none the speed options (FAST / SLOW ROM read, SKIP / FULL drawing) */
static void keys_line(void)
{
    char b[24];
    if (st.pad == 0xFFu) {
        put_s(put_s(b, emu_mode & EMU_FAST_ROM ? "FAST " : "SLOW "), emu_mode & EMU_FRAME_SKIP ? "SKIP" : "FULL");
    } else {
        lsdj_pad_text(b, st.pad);
    }
    core_text(112, (int)STATUS_Y, 128, CORE_FONT_S, b, CORE_THEME, CORE_BG, 0);
    st.pad_shown = st.pad;
}

/* ---- running ---- */
static void lsdj_step(void)
{
    uint32_t t0, us;
    if (core_ms() - st.sec_ms >= 1000u) {
        st.sec_ms += 1000u;
        st.last_fps = st.frames - st.sec_frames;
        st.sec_frames = st.frames;
        st.last_under = ring.under - st.sec_under;
        st.sec_under = ring.under;
        st.last_load = st.last_fps ? st.sec_us / st.last_fps * 100u / 16742u : 0;
        st.sec_us = 0;
        if (st.sec_ms - st.win_ms >= 4000u) {
            st.last_speed = lsdj_speed(st.frames - st.win_frames, st.sec_ms - st.win_ms);
            st.win_ms = st.sec_ms;
            st.win_frames = st.frames;
        }
        status_line();
    }
    if (st.pad != st.pad_shown)
        keys_line();
    if (lsdj_ring_free(&ring) < LSDJ_FRAME_SAMPLES) {     /* ahead of the audio: wait for it */
        st.waits++;
        return;
    }
    st.pot = core_master_pot();
    st.master_q12 = lsdj_master(&st.knob, st.pot);
    t0 = core_ticks();
    st.buttons = core_buttons();
    st.notes = core_notes();
    st.pad = lsdj_pad(st.buttons, pad_map);
    emu_frame(st.pad);
    lsdj_ring_push(&ring, emu.snd, LSDJ_FRAME_SAMPLES);
    us = (core_ticks() - t0) / CORE_TICKS_PER_US;
    st.frames++;
    st.frame_us_sum += us;
    st.sec_us += us;
    if (us > st.frame_max_us)
        st.frame_max_us = us;
}

/* changed line pairs to the screen, round robin, never waiting: while the last transfer runs, nothing; else one
 * transfer of the next changed line pair and up to PUSH_MAX - 1 changed ones right below it (their spans' union),
 * which goes out over SPI while the next Game Boy frame runs */
static void lsdj_push(void)
{
    uint32_t k, g0, n, x0, x1, a, b, r;
    if (!core_lcd_idle())
        return;
    for (k = 0; k < GROUPS; k++) {            /* only rows marked changed are compared */
        g0 = group;
        group = group + 1u == GROUPS ? 0 : group + 1u;
        if (!(dirty[2u * g0] | dirty[2u * g0 + 1u]))
            continue;
        if (lsdj_span(fb[2u * g0], shown[2u * g0], &x0, &x1))
            break;
        dirty[2u * g0] = dirty[2u * g0 + 1u] = 0;   /* changed back: nothing to send */
    }
    if (k == GROUPS)
        return;                               /* the screen shows the frame */
    for (n = 1; n < PUSH_MAX && g0 + n < GROUPS && (dirty[2u * (g0 + n)] | dirty[2u * (g0 + n) + 1u]) &&
                lsdj_span(fb[2u * (g0 + n)], shown[2u * (g0 + n)], &a, &b); n++) {
        x0 = a < x0 ? a : x0;
        x1 = b > x1 ? b : x1;
    }
    for (k = 0; k < n; k++)
        lsdj_scale(pushbuf[buf_i] + k * 3u * LSDJ_SX(x1 - x0), fb[2u * (g0 + k)], x0, x1, PAL);
    core_lcd_blit((int)LSDJ_SX(x0), (int)(Y0 + 3u * g0), (int)LSDJ_SX(x1 - x0), (int)(3u * n), pushbuf[buf_i]);
    buf_i ^= 1u;
    for (r = 2u * g0; r < 2u * (g0 + n); r++) {
        memcpy(&shown[r][x0], &fb[r][x0], x1 - x0);
        dirty[r] = 0;                         /* the span covers all of these rows' changes */
    }
    group = g0 + n == GROUPS ? 0 : g0 + n;
    st.pushes++;
}

static __attribute__((noinline)) void lsdj_start(void)
{
    int r = lsdj_unpack(sram, LSDJ_SRAM, lsdj_boot_save, sizeof lsdj_boot_save);
    core_fill(0, 0, 240, 240, CORE_BG);
    if (r) {
        st.error = -2;
        core_text(0, 110, 240, CORE_FONT_M, "START SAVE DAMAGED", CORE_TEXT, CORE_BG, 1);
        return;
    }
    memset(fb, 0, sizeof fb);
    memset(shown, 0xFF, sizeof shown);        /* no shade: every line pair is sent once */
    memset(dirty, 1, sizeof dirty);
    lsdj_ring_init(&ring, ring_buf, RING_N);
    r = emu_init(lsdj_rom, LSDJ_ROM_BANKS, sram, fb);
    emu.dirty = dirty;
    if (r) {
        st.error = r;
        core_text(0, 110, 240, CORE_FONT_M, "NOT AN LSDJ ROM", CORE_TEXT, CORE_BG, 1);
        return;
    }
    for (r = 0; r < 8; r++)
        pad_map[r] = core_button_bit(KEYS[r]);
    st.start_ms = st.sec_ms = st.win_ms = core_ms();
    st.pad = 0xFF;
    st.pad_shown = 0;                         /* draw the keys part once */
    draw = lsdj_push;
    run = lsdj_step;
}

/* ---- the core's hooks (named by the BLE test; see the top) ---- */
void ble_key(uint32_t pressed_buttons, int32_t select_steps)
{
    (void)select_steps;
    if (!run) {
        if (!st.error && (pressed_buttons & core_play_bit()))
            lsdj_start();
        return;
    }
    if (pressed_buttons & core_button_bit(CORE_BTN_FX))           /* the speed options, switched while running */
        emu_mode ^= EMU_FRAME_SKIP;
    if (pressed_buttons & core_button_bit(CORE_BTN_LFO))
        emu_mode ^= EMU_FAST_ROM;
    if (pressed_buttons & (core_button_bit(CORE_BTN_FX) | core_button_bit(CORE_BTN_LFO)))
        st.pad_shown = (uint8_t)~st.pad;     /* redraw the options */
}

void ble_service(void)
{
    void (*f)(void) = run;
    if (f)
        f();
}

void ble_draw(void)
{
    void (*f)(void) = draw;
    if (f) {
        uint32_t t0 = core_ticks();
        f();
        st.push_ticks += core_ticks() - t0;
        st.push_calls++;
        return;
    }
    if (splash_done || st.error)
        return;
    splash_done = 1;
    core_text(0, 160, 240, CORE_FONT_S, "PLAY: START, THEN REC: DEMO", CORE_THEME, CORE_BG, 1);
    core_text(0, 176, 240, CORE_FONT_S, "UP SCL    DOWN SAVE", CORE_MID, CORE_BG, 1);
    core_text(0, 192, 240, CORE_FONT_S, "LEFT HOME    RIGHT ARP", CORE_MID, CORE_BG, 1);
    core_text(0, 208, 240, CORE_FONT_S, "A GLO    B EDIT", CORE_MID, CORE_BG, 1);
    core_text(0, 224, 240, CORE_FONT_S, "SELECT PLAY    START REC", CORE_MID, CORE_BG, 1);
}

void ble_status(void)                          /* the console's `ble` */
{
    char b[96], *p = b;
    p = put_s(p, run ? "lsdj running " : "lsdj stopped ");
    p = put_s(p, "error ");
    if (st.error < 0) {
        p = put_s(p, "-");
        p = put_u(p, (uint32_t)-st.error);
    } else {
        p = put_u(p, (uint32_t)st.error);
    }
    p = put_s(p, " emu_errors ");
    p = put_u(p, emu.errors);
    put_s(p, "\r\n");
    core_con_puts(b);
    p = put_s(b, "frames ");
    p = put_u(p, st.frames);
    p = put_s(p, " avg_us ");
    p = put_u(p, st.frames ? st.frame_us_sum / st.frames : 0);
    p = put_s(p, " max_us ");
    p = put_u(p, st.frame_max_us);
    p = put_s(p, " fps ");
    p = put_u(p, st.last_fps);
    p = put_s(p, " waits ");
    p = put_u(p, st.waits);
    put_s(p, "\r\n");
    core_con_puts(b);
    p = put_s(b, "keys buttons 0x");
    p = put_hex(p, st.buttons);
    p = put_s(p, " notes 0x");
    p = put_hex(p, st.notes);
    p = put_s(p, " play 0x");
    p = put_hex(p, core_play_bit());
    p = put_s(p, " rec 0x");
    p = put_hex(p, core_rec_bit());
    p = put_s(p, " pad 0x");
    p = put_hex(p, st.pad);
    put_s(p, "\r\n");
    core_con_puts(b);
    p = put_s(b, "master pot ");
    p = put_u(p, st.pot < 0 ? 9999u : (uint32_t)st.pot);
    p = put_s(p, " gain_q12 ");
    p = put_u(p, st.master_q12);
    put_s(p, "\r\n");
    core_con_puts(b);
    p = put_s(b, "mode ");
    p = put_u(p, emu_mode);
    p = put_s(p, " (1 fast rom, 2 frame skip) load% ");
    p = put_u(p, st.last_load);
    put_s(p, "\r\n");
    core_con_puts(b);
    p = put_s(b, "audio gaps ");
    p = put_u(p, ring.under);
    p = put_s(p, " fill ");
    p = put_u(p, lsdj_ring_fill(&ring));
    p = put_s(p, " pushes ");
    p = put_u(p, st.pushes);
    p = put_s(p, " push_avg_us ");
    p = put_u(p, st.push_calls ? st.push_ticks / CORE_TICKS_PER_US / st.push_calls : 0);
    p = put_s(p, " s ");
    p = put_u(p, (core_ms() - st.start_ms) / 1000u);
    put_s(p, "\r\n");
    core_con_puts(b);
}

int ble_started(void) { return run != 0; }
