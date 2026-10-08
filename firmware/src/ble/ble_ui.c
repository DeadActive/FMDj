/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
/* The BLE test's screen, keys and console report (ble.c's unit, after ble_central.c). 240 x 240, AF_S rows of 15 px
 * (its box height): row 0 title and state, row 1 counters and the last error, rows 2-14 the device list or the
 * connection, row 15 the keys. PLAY: start the radio, then scan; SELECT: choose a device; REC: connect / disconnect. */
#define ROW 15
#define LIST_ROWS 13u
#define MON_LINES 10u

static const char *const STATE_NAME[] = {"OFF", "STARTING", "IDLE", "SCANNING", "CONNECTING", "DISCOVERING",
                                         "SUBSCRIBED", "RECEIVING", "FAILED"};
static uint32_t ui_sel, ui_dirty = 1, ui_last_ms, ui_last_st = 0xFFu, ui_last_sig, ui_cleared;
static char mon[MON_LINES][32];                    /* the last monitor lines, a ring */
static uint32_t mon_n, mon_sx;

void ble_key(uint32_t pressed, int32_t select_steps)   /* the only entry from the core (gate 4) */
{
    uint32_t play = pressed & core_play_bit(), rec = pressed & core_rec_bit();
    uint8_t idx[BLE_SCAN_MAX];
    uint32_t n = ble_scan_sorted(&scan, idx);
    if (select_steps) {
        int32_t s = (int32_t)ui_sel + select_steps;
        ui_sel = n ? (uint32_t)(s < 0 ? 0 : s >= (int32_t)n ? (int32_t)n - 1 : s) : 0u;
        ui_dirty = 1;
    }
#if FELUCCA_BLE_STEP >= 2
    if (play) {
        if (st == BLE_OFF)
            ble_start();
        else if (st == BLE_IDLE)
            ble_scan();
        ui_sel = 0;
        ui_dirty = 1;
    }
#else
    (void)play;
#endif
#if FELUCCA_BLE_STEP >= 4
    if (rec) {
        if (st == BLE_IDLE && n)
            ble_connect(ui_sel);
        else if (st >= BLE_CONNECTING && st <= BLE_RECEIVING)
            ble_disconnect();
        mon_n = 0;
        ui_dirty = 1;
    }
#else
    (void)rec;
#endif
}

static void ui_midi(void)                          /* BLE-MIDI to the monitor and USB (step 5) */
{
    uint32_t pkt;
    while (ble_midi_take(&pkt)) {
#if FELUCCA_BLE_STEP >= 5
        char t[32];
        core_midi_out(pkt);
        if (ble_midi_text(pkt, &mon_sx, t)) {
            memcpy(mon[mon_n++ % MON_LINES], t, sizeof t);
            ui_dirty = 1;
        }
#endif
    }
}

static char *put_dev(char *o, const ble_dev_t *d)  /* the name, or the address most significant byte first */
{
    uint32_t i;
    if (d->name[0])
        return put_s(o, d->name);
    for (i = 6; i--;) {
        o = put_x(o, d->addr[i]);
        if (i)
            *o++ = ':';
    }
    *o = 0;
    return o;
}
static char *put_i(char *o, int32_t v) { if (v < 0) { *o++ = '-'; v = -v; } return put_u(o, (uint32_t)v); }

static void row(uint32_t r, const char *s, int fg)
{
    core_text(4, (int)(r * ROW), 232, CORE_FONT_S, s, fg, CORE_BG, 0);
}

void ble_draw(void)
{
    char b[64], *o;
    uint32_t now = core_ms(), code, i, n, first, sig = scan.reports ^ bm.msgs << 12 ^ bm.errors << 24 ^ ui_sel << 28;
    uint8_t idx[BLE_SCAN_MAX];
    const char *err = ble_err(&code);
    if (!ui_dirty && st == ui_last_st && (sig == ui_last_sig || now - ui_last_ms < 100u))
        return;
    ui_dirty = 0;
    ui_last_st = st;
    ui_last_sig = sig;
    ui_last_ms = now;
    if (!ui_cleared) {                             /* the boot splash */
        core_fill(0, 0, 240, 240, CORE_BG);
        ui_cleared = 1;
    }
    core_text(4, 0, 150, CORE_FONT_S, "BLE MIDI TEST", CORE_THEME, CORE_BG, 0);
    core_text(154, 0, 82, CORE_FONT_S, STATE_NAME[st], CORE_TEXT, CORE_BG, 0);
    o = put_u(put_s(b, "REPORTS "), scan.reports);
    if (err[0]) {
        o = put_s(put_s(o, "  "), err);
        if (code)
            o = put_x(put_s(o, " 0x"), code);
    }
    *o = 0;
    row(1, b, err[0] ? CORE_THEME : CORE_MID);
    n = ble_scan_sorted(&scan, idx);
    if (st >= BLE_CONNECTING && st <= BLE_RECEIVING) {
        o = put_dev(put_s(b, "DEVICE "), &scan.dev[conn_dev]);
        row(2, b, CORE_TEXT);
        o = put_u(put_s(b, "MSG "), bm.msgs);
        o = put_u(put_s(o, "  PKT "), bm.packets);
        o = put_u(put_s(o, "  ERR "), bm.errors);
        *o = 0;
        row(3, b, CORE_TEXT);
        row(4, "", CORE_TEXT);
        for (i = 0; i < MON_LINES; i++) {         /* oldest at the top */
            uint32_t k = mon_n + i;
            row(5u + i, mon_n + i >= MON_LINES ? mon[k % MON_LINES] : "", CORE_TEXT);
        }
        row(15, "REC disconnect", CORE_MID);
        return;
    }
    first = ui_sel >= LIST_ROWS ? ui_sel - (LIST_ROWS - 1u) : 0u;
    for (i = 0; i < LIST_ROWS; i++) {
        uint32_t k = first + i, y = (2u + i) * ROW;
        int sel = k == ui_sel && k < n, fg = sel ? CORE_INK : CORE_TEXT, bg = sel ? CORE_SEL : CORE_BG;
        const ble_dev_t *d;
        if (FELUCCA_BLE_STEP < 3 || k >= n) {
            core_fill(0, (int)y, 240, ROW, CORE_BG);
            continue;
        }
        d = &scan.dev[idx[k]];
        core_fill(0, (int)y, 240, ROW, bg);
        put_dev(b, d);
        core_text(4, (int)y, 150, CORE_FONT_S, b, fg, bg, 0);
        *put_i(b, d->rssi) = 0;
        core_text(158, (int)y, 40, CORE_FONT_S, b, fg, bg, 0);
        core_text(200, (int)y, 36, CORE_FONT_S, d->midi ? "MIDI" : "", sel ? CORE_INK : CORE_THEME, bg, 0);
    }
    row(15, st == BLE_OFF ? "PLAY start" : st == BLE_IDLE ? (FELUCCA_BLE_STEP >= 4 ? "PLAY scan  SEL choose  REC connect"
                                                                                    : "PLAY scan  SEL choose") : "",
        CORE_MID);
}

void ble_status(void)                              /* console `ble` */
{
    char b[96], *o;
    uint32_t code, i, n;
    uint8_t idx[BLE_SCAN_MAX];
    const char *err = ble_err(&code);
    o = put_u(put_s(b, "ble: step "), FELUCCA_BLE_STEP);
    o = put_s(put_s(o, " state "), STATE_NAME[st]);
    o = put_u(put_s(o, " reports "), scan.reports);
    o = put_u(put_s(o, " msgs "), bm.msgs);
    o = put_u(put_s(o, " pkts "), bm.packets);
    o = put_u(put_s(o, " errs "), bm.errors);
    o = put_u(put_s(o, " lost "), ring_lost);
    o = put_s(o, "\r\n");
    *o = 0;
    core_con_puts(b);
    o = put_s(b, "  mac ");
    for (i = 6; i--;)
        o = put_x(o, ble_mac[i]);
    if (err[0])
        o = put_x(put_s(put_s(put_s(o, "  error "), err), " 0x"), code);
    o = put_s(o, "\r\n");
    *o = 0;
    core_con_puts(b);
    if (ble_diag.magic == BLE_DIAG_MAGIC) {     /* kept over a reboot: where the last start got to */
        o = put_u(put_s(b, "  starts "), ble_diag.starts);
        o = put_u(put_s(o, " stage "), ble_diag.stage);
        o = put_u(put_s(o, " task "), ble_diag.task);
        o = put_u(put_s(o, " trim "), ble_diag.trim);
        o = put_x(put_x(put_x(put_x(put_s(o, " irqs 0x"), ble_irqs_requested[1] >> 8), ble_irqs_requested[1]),
                        ble_irqs_requested[0] >> 24), ble_irqs_requested[0] >> 16);
        o = put_x(put_x(o, ble_irqs_requested[0] >> 8), ble_irqs_requested[0]);
        o = put_s(o, "\r\n");
        *o = 0;
        core_con_puts(b);
    }
    if (fm1_crash.magic == 0x43525348u) {        /* the core's crash record (fm1_irq.h), all of it */
        static const char *const F[] = {"vec", "pc", "rets", "emu", "dbg", "sp", "psr", "icfg", "uptime_ms",
                                        "etm0", "etm1", "etm2", "etm3", "early"};
        const uint32_t *v = &fm1_crash.vec;
        core_con_puts("  crash");
        for (i = 0; i < 14u; i++) {
            o = put_s(put_s(put_s(b, " "), F[i]), " ");
            o = put_x(put_x(put_x(put_x(o, v[i] >> 24), v[i] >> 16), v[i] >> 8), v[i]);
            *o = 0;
            core_con_puts(b);
        }
        core_con_puts("\r\n");
    }
    for (i = 0; i < 4u; i++) {                     /* the task stacks' unused bytes (4096 each) */
        const char *name;
        uint32_t f = ble_os_stack_free(i, &name);
        if (!name)
            continue;
        o = put_u(put_s(put_s(put_s(b, "  task "), name), " stack free "), f);
        o = put_s(o, "\r\n");
        *o = 0;
        core_con_puts(b);
    }
    n = ble_scan_sorted(&scan, idx);
    for (i = 0; i < n; i++) {
        const ble_dev_t *d = &scan.dev[idx[i]];
        uint32_t k;
        o = put_s(b, "  ");
        for (k = 6; k--;)
            o = put_x(o, d->addr[k]);
        o = put_u(put_s(o, " t"), d->addr_type);
        o = put_i(put_s(o, " "), d->rssi);
        o = put_s(o, d->midi ? " MIDI " : " ");
        o = put_s(o, d->name);
        o = put_s(o, "\r\n");
        *o = 0;
        core_con_puts(b);
    }
}
