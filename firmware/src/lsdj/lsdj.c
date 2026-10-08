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
#define PUSH_AHEAD 8u                         /* line pairs sent per pass while the audio is ahead ... */
#define PUSH_BEHIND 2u                        /* ... and while it is not */
#define STATUS_Y 223u

static uint8_t sram[LSDJ_SRAM] LSDJ_POOL;
static uint8_t fb[LSDJ_H][LSDJ_W] LSDJ_POOL, shown[LSDJ_H][LSDJ_W] LSDJ_POOL;
static int16_t ring_buf[2u * RING_N] LSDJ_POOL;
static uint16_t pushbuf[2][3u * 240u];        /* DMA sources: one is filled while the other goes out */
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
    uint32_t frames, waits, frame_max_us, frame_us_sum, pushes, start_ms;
    uint32_t sec_ms, sec_frames, sec_under, last_fps, last_under;
    uint32_t buttons, notes;                  /* as read for the last frame (console `ble`) */
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

static void status_line(void)                  /* left: speed, frames per second, audio gaps */
{
    char b[48], *p = b;
    p = put_u(p, st.last_fps * 100u / 60u);
    p = put_s(p, "% ");
    p = put_u(p, st.last_fps);
    p = put_s(p, "FPS GAP");
    p = put_u(p, st.last_under * 100u / 44100u);
    put_s(p, "%");
    core_text(0, (int)STATUS_Y, 112, CORE_FONT_S, b, CORE_MID, CORE_BG, 0);
}

static void keys_line(void)                    /* right: the Game Boy keys held (redrawn when they change) */
{
    char b[24];
    lsdj_pad_text(b, st.pad);
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
        status_line();
    }
    if (st.pad != st.pad_shown)
        keys_line();
    if (lsdj_ring_free(&ring) < LSDJ_FRAME_SAMPLES) {     /* ahead of the audio: wait for it */
        st.waits++;
        return;
    }
    t0 = core_ticks();
    st.buttons = core_buttons();
    st.notes = core_notes();
    st.pad = lsdj_pad(st.buttons, pad_map);
    emu_frame(st.pad);
    lsdj_ring_push(&ring, emu.snd, LSDJ_FRAME_SAMPLES);
    us = (core_ticks() - t0) / CORE_TICKS_PER_US;
    st.frames++;
    st.frame_us_sum += us;
    if (us > st.frame_max_us)
        st.frame_max_us = us;
}

static void lsdj_push(void)                    /* changed line pairs to the screen, round robin */
{
    uint32_t budget = lsdj_ring_fill(&ring) >= 2u * LSDJ_FRAME_SAMPLES ? PUSH_AHEAD : PUSH_BEHIND, k, x0, x1, r;
    for (k = 0; k < GROUPS && budget; k++) {
        uint32_t g = group;
        group = group + 1u == GROUPS ? 0 : group + 1u;
        if (!lsdj_span(fb[2u * g], shown[2u * g], &x0, &x1))
            continue;
        lsdj_scale(pushbuf[buf_i], fb[2u * g], x0, x1, PAL);
        core_lcd_blit((int)LSDJ_SX(x0), (int)(Y0 + 3u * g), (int)LSDJ_SX(x1 - x0), 3, pushbuf[buf_i]);
        buf_i ^= 1u;
        for (r = 0; r < 2u; r++)
            memcpy(&shown[2u * g + r][x0], &fb[2u * g + r][x0], x1 - x0);
        st.pushes++;
        budget--;
    }
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
    lsdj_ring_init(&ring, ring_buf, RING_N);
    r = emu_init(lsdj_rom, LSDJ_ROM_BANKS, sram, fb);
    if (r) {
        st.error = r;
        core_text(0, 110, 240, CORE_FONT_M, "NOT AN LSDJ ROM", CORE_TEXT, CORE_BG, 1);
        return;
    }
    for (r = 0; r < 8; r++)
        pad_map[r] = core_button_bit(KEYS[r]);
    st.start_ms = st.sec_ms = core_ms();
    st.pad = 0xFF;
    st.pad_shown = 0;                         /* draw the keys part once */
    draw = lsdj_push;
    run = lsdj_step;
}

/* ---- the core's hooks (named by the BLE test; see the top) ---- */
void ble_key(uint32_t pressed_buttons, int32_t select_steps)
{
    (void)select_steps;
    if (!run && !st.error && (pressed_buttons & core_play_bit()))
        lsdj_start();
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
        f();
        return;
    }
    if (splash_done || st.error)
        return;
    splash_done = 1;
    core_text(0, 160, 240, CORE_FONT_S, "PLAY: START LSDJ", CORE_THEME, CORE_BG, 1);
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
    p = put_s(b, "audio gaps ");
    p = put_u(p, ring.under);
    p = put_s(p, " fill ");
    p = put_u(p, lsdj_ring_fill(&ring));
    p = put_s(p, " pushes ");
    p = put_u(p, st.pushes);
    p = put_s(p, " s ");
    p = put_u(p, (core_ms() - st.start_ms) / 1000u);
    put_s(p, "\r\n");
    core_con_puts(b);
}

int ble_started(void) { return run != 0; }
