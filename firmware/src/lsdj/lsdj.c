/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The LSDj unit (FELUCCA_LSDJ=1): its own object, linked in place of the BLE test unit. The core's unit is
 * compiled exactly as in the BLE test build (FELUCCA_BLE=1: its main-loop hooks ble_key / ble_service / ble_draw,
 * the console's `ble`), so the update path is the one proven on the device (gate 2b,
 * tools/check_update_path.py). The core calls the hooks below; this unit calls the core only through ble_core.h.
 *
 * Nothing runs at boot: PLAY starts the Game Boy (ble_key). From then on, each main-loop pass runs one Game Boy
 * frame when the audio ring has room for its sound (the audio ISR plays the ring: lsdj_audio), then sends a few
 * changed line pairs to the screen (1.5x: 240 x 216). The status line tells how it keeps up: SPD, LOAD, and GAPS,
 * the seconds in which the ring ran dry. The emulator is reached only through function pointers set
 * by lsdj_start (gate 4, tools/check_ble_boot.py --lsdj, walks the direct calls from boot).
 *
 * Songs move over the console: `ble WORDS` is a transfer command (lsdj_xfer.c; web/lsdj.html drives it). The host
 * holds the Game Boy between two frames, reads or writes the save RAM, and lets it run again.
 *
 * Flash (lsdj_store.c: Felucca's user-sample area only): PLAY loads LSDj's save from it (reading only); it is
 * written when REC is held for a second, and for a request the console queued (`ble save`, the kit banks). Both
 * happen in lsdj_step, which the core reaches only through its pointer: no flash write is reachable from boot
 * (gate 4). Custom kits live there too, and show in the ROM's free kit banks (rom_tab).
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
#include "lsdj/lsdj_xfer.c"
#define LSDJ_STORE_FN static __attribute__((noinline))   /* gate 4 finds the flash writers by name */
#include "lsdj/lsdj_store.c"
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
static lsdj_xfer_t xfer;                      /* the console's `ble ...` (lsdj_xfer.c): the page moves songs with it */
static char xfer_out[LSDJ_XFER_OUT];

/* The Game Boy keys (the owner's layout, 2026-10-12):
 *   the cursor: the SELECT knob left / right, the ALGORITHM knob up / down (clockwise: right, down), a step = a press
 *   A, B: the top F and G of the keyboard (lsdj_gb.c LSDJ_NOTE_A / _B);  START: PLAY;  SELECT: SEQ
 *   LSDj's screens (SELECT + direction): the PRESETS knob left / right, OCT+ up, OCT- down
 * and, as before, the cursor on buttons too (they can be held): UP SCL, DOWN SAVE, LEFT HOME, RIGHT ARP.
 * The buttons below by label, in JOYPAD bit order: A, B (no button), SELECT, START, RIGHT, LEFT, UP, DOWN.
 * OCT- + OCT+ held 5 s together is still the core's UBOOT. */
static const int8_t KEYS[8] = {-1, -1, CORE_BTN_SEQ, CORE_BTN_PLAY,
                               CORE_BTN_ARP, CORE_BTN_HOME, CORE_BTN_SCL, CORE_BTN_SAVE};
static lsdj_knobs_t knobs;                    /* knob steps and shortcuts waiting to be pressed (lsdj_gb.c) */
static const lsdj_flash_t flash = {core_flash_ptr, core_flash_erase, core_flash_prog};
static const lsdj_flash_t flash_ro = {core_flash_ptr, 0, 0};   /* PLAY's: it reads, and has nothing to write with */
static lsdj_store_t store;                    /* what the flash holds (lsdj_store.c), scanned by lsdj_start */
static uint8_t flash_on;                      /* the core has flash for us */
static lsdj_bank_t rom_tab[LSDJ_ROM_BANKS];   /* the Game Boy's ROM: lsdj_rom, the flash kit banks in its free ones */
static uint8_t kit_bank[LSDJ_ST_KMAX], nkit;  /* the ROM's free kit banks in order: flash kit bank j shows as kit_bank[j] */
static uint8_t kit_buf[LSDJ_BANK] LSDJ_POOL;  /* a kit bank on its way from the console to flash */

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
    uint8_t held_shown;                       /* "USB" is on the status line */
    char note[12];                            /* shown there instead for 2 s: SAVING, SAVED nnK, SAVE FULL, SAVE FAILED */
    uint32_t note_ms, rec_ms;                 /* since the note; since REC went down (| 1: 0 = up) */
    uint8_t rec_done;                         /* this REC press has saved */
    uint32_t sec_skipped, last_skipped;       /* LSDj's wait loop: instructions not run, a frame (console `ble`) */
    uint32_t gaps;                            /* seconds in which the sound ran dry (the status line's GAPS) */
    uint8_t gap_off;                          /* ... but not this one: the Game Boy was held, started or saved in it */
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

/* right: USB while the host holds the Game Boy; else the Game Boy keys held, or with none GAPS: the seconds so far
 * in which the sound ran dry (the FM-1 was too slow: a gap is heard), and each option that is switched off:
 * SLOW (the ROM read, LFO), FULL (every frame drawn, FX), SPIN (LSDj's wait loop run, EDIT), OLD (the sound per
 * frame, ENV) */
static void keys_line(void)
{
    char b[40], *p;
    if (st.note[0]) {
        put_s(b, st.note);
    } else if (xfer.held) {
        put_s(b, "USB");
    } else if (st.pad == 0xFFu) {
        p = put_u(put_s(b, "GAPS "), st.gaps);
        if (!(emu_mode & EMU_FAST_ROM))
            p = put_s(p, " SLOW");
        if (!(emu_mode & EMU_FRAME_SKIP))
            p = put_s(p, " FULL");
        if (!(emu_mode & EMU_WAIT_SKIP))
            p = put_s(p, " SPIN");
        if (!(emu_mode & EMU_SND_FINE))
            put_s(p, " OLD");
    } else {
        lsdj_pad_text(b, st.pad);
    }
    core_text(112, (int)STATUS_Y, 128, CORE_FONT_S, b, CORE_THEME, CORE_BG, 0);
    st.pad_shown = st.pad;
    st.held_shown = xfer.held;
}

/* ---- running ---- */
/* ---- flash: the save and the kit banks ---- */
static void lsdj_xfer_flash(void)              /* what `ble info` and the kit commands need to know */
{
    xfer.kit = kit_buf;
    xfer.kit_bank = kit_bank;
    xfer.nkit = nkit;
    xfer.kits = store.k < nkit ? store.k : nkit;
    xfer.saved = store.have;
    xfer.cap = lsdj_st_cap(store.k);
    xfer.used = store.have ? store.len : 0;
}

static void lsdj_banks(void)                   /* rom_tab: the ROM, and the K flash kit banks where it has none */
{
    uint32_t b, j;
    memcpy(rom_tab, lsdj_rom, sizeof rom_tab);
    for (b = 8, nkit = 0; b < LSDJ_ROM_BANKS && nkit < LSDJ_ST_KMAX; b++)   /* kits: banks 8..26 and 32.. */
        if ((b <= 26u || b >= 32u) && !lsdj_rom[b].n)
            kit_bank[nkit++] = (uint8_t)b;
    for (j = 0; j < store.k && j < nkit; j++) {   /* (store.k is the flash layout's: never changed here, even if a
                                                   * store made with another ROM has more banks than this one shows) */
        rom_tab[kit_bank[j]].p = core_flash_ptr(lsdj_st_kit_off(j));
        rom_tab[kit_bank[j]].n = LSDJ_BANK;
    }
    emu_hi_set(~0u);                          /* no bank cached */
    lsdj_xfer_flash();
}

static void lsdj_note(const char *s)           /* on the status line now, for 2 s */
{
    put_s(st.note, s);
    st.note_ms = core_ms();
    keys_line();
}

static int lsdj_save(void)                     /* the save RAM into flash: a second or two, nothing else runs */
{
    char b[12];
    int r;
    st.gap_off = 1;                           /* (silence, but not a gap of the Game Boy's) */
    lsdj_note("SAVING");
    r = flash_on ? lsdj_store_save(&flash, &store, sram, LSDJ_SRAM) : LSDJ_ST_FAIL;
    if (r == LSDJ_ST_OK) {
        put_s(put_u(put_s(b, "SAVED "), (store.len + 1023u) / 1024u), "K");
        lsdj_note(b);
    } else {
        lsdj_note(r == LSDJ_ST_FULL ? "SAVE FULL" : "SAVE FAILED");
    }
    lsdj_xfer_flash();
    return r;
}

static void lsdj_flash_req(void)               /* the console queued it (lsdj_xfer.c): flash is written only here */
{
    int r = LSDJ_ST_FAIL;
    st.gap_off = 1;
    if (xfer.req == LSDJ_REQ_SAVE)
        r = lsdj_save();
    else if (!flash_on)
        r = LSDJ_ST_FAIL;
    else if (xfer.req == LSDJ_REQ_KIT)
        r = lsdj_store_kit_write(&flash, &store, xfer.req_arg, kit_buf);
    else if (xfer.req == LSDJ_REQ_KIT_ERASE)
        r = lsdj_store_kit_erase(&flash, &store, xfer.req_arg);
    else if (xfer.req == LSDJ_REQ_KITS)
        r = lsdj_store_kits(&flash, &store, xfer.req_arg, sram, LSDJ_SRAM);
    lsdj_banks();                             /* K may have changed */
    xfer.res = (uint8_t)r;
    xfer.req = LSDJ_REQ_NONE;
}

static void lsdj_restart(void)                 /* `ble go reset`: the Game Boy from its reset, the save RAM kept */
{
    emu_init(rom_tab, LSDJ_ROM_BANKS, sram, fb);      /* (it ran before: the cartridge is LSDj) */
    emu.dirty = dirty;
    memset(shown, 0xFF, sizeof shown);        /* every line pair is sent again */
    memset(dirty, 1, sizeof dirty);
}

static void lsdj_step(void)
{
    uint32_t t0, us;
    if (core_ms() - st.sec_ms >= 1000u) {
        st.sec_ms += 1000u;
        st.last_fps = st.frames - st.sec_frames;
        st.sec_frames = st.frames;
        st.last_under = ring.under - st.sec_under;
        st.sec_under = ring.under;
        if (st.last_under && !st.gap_off) {   /* the sound ran dry in this second, and nobody stopped the Game Boy */
            st.gaps++;
            st.pad_shown = (uint8_t)~st.pad;  /* redraw GAPS */
        }
        st.gap_off = 0;
        st.last_load = st.last_fps ? st.sec_us / st.last_fps * 100u / 16742u : 0;
        st.last_skipped = st.last_fps && emu.skipped >= st.sec_skipped ? (emu.skipped - st.sec_skipped) / st.last_fps : 0;
        st.sec_skipped = emu.skipped;         /* (it starts again with the Game Boy: `ble go reset`) */
        st.sec_us = 0;
        if (st.sec_ms - st.win_ms >= 4000u) {
            st.last_speed = lsdj_speed(st.frames - st.win_frames, st.sec_ms - st.win_ms);
            st.win_ms = st.sec_ms;
            st.win_frames = st.frames;
        }
        status_line();
    }
    lsdj_xfer_tick(&xfer, core_ms());
    if (st.note[0] && core_ms() - st.note_ms >= 2000u) {
        st.note[0] = 0;
        st.held_shown = (uint8_t)!xfer.held;  /* redraw what the note covered */
    }
    if (st.pad != st.pad_shown || xfer.held != st.held_shown)
        keys_line();
    if (xfer.req)
        lsdj_flash_req();
    if (!(core_buttons() & core_rec_bit())) { /* REC held for a second: save (once a press; not under the host) */
        st.rec_ms = 0;
        st.rec_done = 0;
    } else if (!st.rec_ms) {
        st.rec_ms = core_ms() | 1u;
    } else if (!st.rec_done && !xfer.held && core_ms() - st.rec_ms >= 1000u) {
        st.rec_done = 1;
        lsdj_save();
    }
    if (xfer.held) {
        st.gap_off = 1;
        return;                               /* the host reads or writes the save: no Game Boy frame */
    }
    if (xfer.reset_req) {                     /* the save changed under LSDj: it starts again and reads it */
        xfer.reset_req = 0;
        lsdj_restart();
    }
    if (lsdj_ring_free(&ring) < LSDJ_FRAME_SAMPLES) {     /* ahead of the audio: wait for it */
        st.waits++;
        return;
    }
    st.pot = core_master_pot();
    st.master_q12 = lsdj_master(&st.knob, st.pot);
    t0 = core_ticks();
    st.buttons = core_buttons();
    st.notes = core_notes();
    st.pad = lsdj_pad(st.buttons, st.notes, pad_map) & (uint8_t)~lsdj_knobs_frame(&knobs);
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
    int r = -1;
    flash_on = core_flash_ptr(LSDJ_ST_BASE) != 0;
    if (flash_on) {                           /* (reading only: flash is written on REC or a console request) */
        lsdj_store_scan(&flash_ro, &store);
        if (store.have)
            r = lsdj_store_load(&flash_ro, &store, sram, LSDJ_SRAM);
    }
    if (r)                                    /* no save in flash: the start-up save, as built */
        r = lsdj_unpack(sram, LSDJ_SRAM, lsdj_boot_save, sizeof lsdj_boot_save);
    lsdj_banks();
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
    r = emu_init(rom_tab, LSDJ_ROM_BANKS, sram, fb);
    emu.dirty = dirty;
    if (r) {
        st.error = r;
        core_text(0, 110, 240, CORE_FONT_M, "NOT AN LSDJ ROM", CORE_TEXT, CORE_BG, 1);
        return;
    }
    for (r = 0; r < 8; r++)
        pad_map[r] = KEYS[r] < 0 ? 0 : core_button_bit(KEYS[r]);
    st.start_ms = st.sec_ms = st.win_ms = core_ms();
    st.pad = 0xFF;
    st.pad_shown = 0;                         /* draw the keys part once */
    st.gaps = 0;
    st.gap_off = 1;                           /* (the ring is empty until the first frame: not a gap) */
    draw = lsdj_push;
    run = lsdj_step;
    xfer.started = 1;
}

/* ---- the core's hooks (named by the BLE test; see the top) ---- */
void ble_key(uint32_t pressed_buttons, int32_t select_steps)
{
    int32_t algo = core_enc_steps(CORE_ENC_ALGO), preset = core_enc_steps(CORE_ENC_PRESET);   /* (always taken) */
    if (!run) {
        if (!st.error && (pressed_buttons & core_play_bit()))
            lsdj_start();
        return;
    }
    if (xfer.held) {                          /* the host has the Game Boy: turns are not kept for later */
        memset(&knobs, 0, sizeof knobs);
    } else {
        lsdj_knobs_add(&knobs, LSDJ_AX_LR, select_steps);
        lsdj_knobs_add(&knobs, LSDJ_AX_UD, algo);
        lsdj_knobs_add(&knobs, LSDJ_AX_NAV_LR, preset);
        if (pressed_buttons & core_button_bit(CORE_BTN_OCTUP))
            lsdj_knobs_add(&knobs, LSDJ_AX_NAV_UD, -1);
        if (pressed_buttons & core_button_bit(CORE_BTN_OCTDN))
            lsdj_knobs_add(&knobs, LSDJ_AX_NAV_UD, 1);
    }
    if (pressed_buttons & core_button_bit(CORE_BTN_FX))           /* the options, switched while running */
        emu_mode ^= EMU_FRAME_SKIP;
    if (pressed_buttons & core_button_bit(CORE_BTN_LFO))
        emu_mode ^= EMU_FAST_ROM;
    if (pressed_buttons & core_button_bit(CORE_BTN_EDIT))         /* (LOAD shows what the wait loop costs) */
        emu_mode ^= EMU_WAIT_SKIP;
    if (pressed_buttons & core_button_bit(CORE_BTN_ENV))          /* (to hear the sound per frame against it) */
        emu_mode ^= EMU_SND_FINE;
    if (pressed_buttons & (core_button_bit(CORE_BTN_FX) | core_button_bit(CORE_BTN_LFO) |
                           core_button_bit(CORE_BTN_EDIT) | core_button_bit(CORE_BTN_ENV)))
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
    core_text(0, 160, 240, CORE_FONT_S, "PLAY: START, THEN PLAY: DEMO", CORE_THEME, CORE_BG, 1);
    core_text(0, 176, 240, CORE_FONT_S, "LEFT RIGHT: SELECT KNOB", CORE_MID, CORE_BG, 1);
    core_text(0, 192, 240, CORE_FONT_S, "UP DOWN: ALGORITHM KNOB", CORE_MID, CORE_BG, 1);
    core_text(0, 208, 240, CORE_FONT_S, "A: TOP F  B: TOP G  SEL: SEQ", CORE_MID, CORE_BG, 1);
    core_text(0, 224, 240, CORE_FONT_S, "SCREENS: PRESETS, OCT- OCT+", CORE_MID, CORE_BG, 1);
}

static void lsdj_console(const char *args)     /* the words after `ble` on the console line: a transfer command */
{
    xfer.sram = sram;
    xfer.size = LSDJ_SRAM;
    xfer.rom = run ? rom_tab : lsdj_rom;    /* (the flash kit banks are in it once PLAY has read the store) */
    xfer.nbanks = LSDJ_ROM_BANKS;
    lsdj_xfer_cmd(&xfer, args, core_ms(), xfer_out);
    core_con_puts(xfer_out);
    core_con_puts("\r\n");
}

void ble_status(void)                          /* the console's `ble`: the status; `ble WORDS`: a transfer command */
{
    char b[96], *p = b;
    const char *a = core_con_line();          /* (console.c is not ours to change: gate 2b; it calls this for `ble`) */
    while (*a == ' ')
        a++;
    if (a[0] == 'b' && a[1] == 'l' && a[2] == 'e')
        a += 3;
    while (*a == ' ')
        a++;
    if (*a) {
        lsdj_console(a);
        return;
    }
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
    p = put_s(p, " (1 fast rom, 2 frame skip, 4 wait skip, 8 fine sound) load% ");
    p = put_u(p, st.last_load);
    put_s(p, "\r\n");
    core_con_puts(b);
    p = put_s(b, "gap seconds ");
    p = put_u(p, st.gaps);
    p = put_s(p, " wait skipped ");                   /* instructions of LSDj's wait loop not run, a frame */
    p = put_u(p, st.last_skipped);
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
