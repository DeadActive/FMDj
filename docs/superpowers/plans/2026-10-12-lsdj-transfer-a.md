# LSDj song transfer (plan A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Chrome page uploads `.lsdprj` / `.lsdsng` songs to LSDj running on the FM-1 and downloads songs as `.lsdprj` and the whole save as `.sav`, over the USB serial console.

**Architecture:** The device only moves bytes: words after the console's existing `ble` (`ble hold`, `ble r ...`; pure parser in `lsdj_xfer.c`) read and write the save RAM and read ROM banks while the Game Boy is held. All format logic is one JavaScript module (`web/lsdjfs.js`); a second module (`web/lsdjlink.js`) speaks the console protocol and runs the page's flows; `web/lsdj.html` is the page. Nothing touches flash (plan B).

**Tech Stack:** C (freestanding, pi32v2 `-Os`; host tests with `cc`), JavaScript ES modules (node tests, Web Serial in Chrome), Python 3 (`web/make_site.py`).

**Spec:** `docs/superpowers/specs/2026-10-11-lsdj-transfer-save-design.md` (plan A = its "Part 1" and "Part 3" without `k*`, `kits`, `save` and the flash-use bar; the spec's `lsdj WORDS` is `ble WORDS` on the wire).

## Global Constraints

- Work in the worktree `~/Desktop/dev/fm1-lsdj-xfer`, branch `lsdj-xfer`. Never push: the only remote is upstream Felucca.
- The updater stays reachable: `tests/ble/update_path.ref` is never rewritten. Gate 2b must print `update path: unchanged` for every firmware build. Do not edit `console.c`, `core_main.c`, `usb.c`, `ota.c`, `ota_hw.c`, `libc.c`, `storage.c`, `storage_hw.c`, `firmware/hal/*`, `firmware/loader/*` (`console.c` compiles into `fm1_main`, which the gate compares: measured).
- Nothing new runs at boot: gate 4 must print `no Game Boy reachable at boot (only through ble_key)`. The console handler must not call `emu_init`, `emu_frame`, `gb_*` or `lsdj_start` / `lsdj_step` / `lsdj_push` directly.
- Private build: the ROM, `build/` and everything made from the ROM are never committed. Run `git status --short` before each commit; only the files named in the task may appear.
- No flash writes anywhere in this plan.
- File headers: `SPDX-License-Identifier: GPL-3.0-only` and `LSDj fork: 2026 DEADACTIVE`.
- Numbers on the wire are hex without a prefix. Read replies carry at most 256 bytes, write lines at most 20 (the console line is 64 characters and cannot grow).
- Firmware build command (Docker must be running):
  `PYTHON=$HOME/Desktop/dev/fm1-lsdj/build/venv/bin/python FELUCCA_LSDJ=1 LSDJ_ROM=$HOME/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb ./build.sh`
- Tests that need the ROM read `LSDJ_ROM` and skip without it; export it as above when running them.

## Review Focus

1. **USB unplugged or an error in the middle of an upload:** LSDj's file system must stay as it was (the block table is written last) and the Game Boy must run again. Test: Task 4, "a failing write leaves the block table alone".
2. **LSDj not started (PLAY not pressed) when the page is used:** the person gets "press PLAY on the FM-1", not an error code. Test: Task 4, "not started is said plainly".
3. **A file that is not a song is dropped (a `.sav`, a picture, a truncated download):** refused on the Mac before any command is sent. Test: Task 4, "a damaged file sends nothing".
4. **A song saved in LSDj after the page drew its list:** the upload must use the header it reads under hold, not the list on screen. Test: Task 4, "an upload reads the header again".
5. **A song whose patched kit number would become 0xC0 or 0xE0 (the packing's escape bytes):** refused, never written (LSDPatcher has this hole). Test: Task 3, "a renumbered kit byte never becomes an escape".

---

### Task 1: The transfer commands (`lsdj_xfer.c`)

**Files:**
- Create: `firmware/src/lsdj/lsdj_xfer.c`
- Create: `tests/lsdj/lsdj_xfer_test.c`
- Modify: `tests/run_tests.sh` (one line after the `lsdj_gb_test` line)

**Interfaces:**
- Consumes: `lsdj_bank_t`, `lsdj_rom_byte`, `LSDJ_BANK` from `firmware/src/lsdj/lsdj_gb.c`.
- Produces:
  - `typedef struct { uint8_t *sram; uint32_t size; const lsdj_bank_t *rom; uint32_t nbanks; uint8_t started, held, reset_req; uint32_t last_ms; } lsdj_xfer_t;`
  - `static void lsdj_xfer_cmd(lsdj_xfer_t *x, const char *p, uint32_t now_ms, char *out);` (`out`: `LSDJ_XFER_OUT` bytes, one reply line, no line end)
  - `static void lsdj_xfer_tick(lsdj_xfer_t *x, uint32_t now_ms);`
  - `LSDJ_XFER_OUT` (521), `LSDJ_XFER_RD` (256), `LSDJ_XFER_WR` (20), `LSDJ_XFER_IDLE_MS` (5000)
  - the vectors file `build/tests/lsdj_xfer_vectors.txt`: lines `> COMMAND`, `< REPLY`, and `! started` where the test sets `started`
  - the test state other tasks reproduce: save byte `i` = `(i * 7 + (i >> 8)) & 0xFF`; 3 ROM banks, bank `b` stores `100 * (b + 1)` bytes, byte `j` = `(b * 31 + j) & 0xFF`

- [ ] **Step 1: Write the failing test**

Create `tests/lsdj/lsdj_xfer_test.c`:

```c
/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* Host test of the LSDj unit's transfer commands (firmware/src/lsdj/lsdj_xfer.c). It also writes every command and
 * reply to build/tests/lsdj_xfer_vectors.txt (the words after `ble`): web/test_lsdj.mjs replays them against its
 * simulated device, so the page's tests and the firmware cannot drift apart.
 *   cc -Wall -O1 -Ifirmware/src -o build/tests/lsdj_xfer_test tests/lsdj/lsdj_xfer_test.c */
#include <stdio.h>
#include <string.h>
#include "lsdj/lsdj_gb.c"
#include "lsdj/lsdj_xfer.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define NB 3u
static uint8_t sram[0x20000], bank[NB][LSDJ_BANK];
static lsdj_bank_t rom[NB];
static lsdj_xfer_t x;
static char out[LSDJ_XFER_OUT + 64];
static FILE *vec;
static uint32_t now;

static const char *say(const char *cmd)
{
    memset(out, 0x55, sizeof out);
    lsdj_xfer_cmd(&x, cmd, now, out);
    CHECK(strlen(out) < LSDJ_XFER_OUT);                /* the reply fits its buffer */
    CHECK((uint8_t)out[LSDJ_XFER_OUT] == 0x55);
    if (vec)
        fprintf(vec, "> %s\n< %s\n", cmd, out);
    return out;
}
static int is(const char *cmd, const char *reply) { return !strcmp(say(cmd), reply); }

static const char *data(const uint8_t *d, uint32_t n)  /* "ok HEX CRC", by the test's own printing */
{
    static char b[LSDJ_XFER_OUT + 8];
    char *o = b + sprintf(b, "ok ");
    uint32_t i;
    for (i = 0; i < n; i++)
        o += sprintf(o, "%02X", d[i]);
    sprintf(o, " %04X", lsdj_crc16(d, n));
    return b;
}

static uint32_t ref_crc32(const uint8_t *p, uint32_t n)   /* bit by bit: not the unit's table */
{
    uint32_t c = 0xFFFFFFFFu;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void setup(void)
{
    uint32_t i, b;
    for (i = 0; i < sizeof sram; i++)
        sram[i] = (uint8_t)(i * 7u + (i >> 8));
    for (b = 0; b < NB; b++) {
        memset(bank[b], 0xEE, LSDJ_BANK);              /* bytes past n must never be read */
        rom[b].p = bank[b];
        rom[b].n = (uint16_t)(100u * (b + 1u));
        for (i = 0; i < rom[b].n; i++)
            bank[b][i] = (uint8_t)(b * 31u + i);
    }
    memset(&x, 0, sizeof x);
    x.sram = sram;
    x.size = sizeof sram;
    x.rom = rom;
    x.nbanks = NB;
}

static void test_crc(void)
{
    uint32_t c = 0xFFFFFFFFu, i;
    CHECK(lsdj_crc16((const uint8_t *)"123456789", 9) == 0x31C3u);      /* CRC-16/XMODEM's check value */
    for (i = 0; i < 9u; i++)
        c = lsdj_crc32_add(c, (uint8_t)"123456789"[i]);
    CHECK(~c == 0xCBF43926u);                                            /* CRC-32's check value */
}

static void test_state(void)
{
    CHECK(is("info", "ok v1 started 0 held 0 banks 03"));
    CHECK(is("hold", "err not started"));
    CHECK(is("r 0 4", "err hold"));
    CHECK(is("w 0 00 0000", "err hold"));
    if (vec)
        fprintf(vec, "! started\n");
    x.started = 1;
    CHECK(is("  hold", "ok"));                          /* leading spaces: as the console passes the line */
    CHECK(is("info", "ok v1 started 1 held 1 banks 03"));
    CHECK(is("info x", "err ?"));
    CHECK(is("", "err ?"));
    CHECK(is("holdx", "err ?"));
    CHECK(is("format", "err ?"));
}

static void test_read(void)
{
    uint8_t d[4] = {0x00, 0x07, 0x0E, 0x15};
    CHECK(is("r 0 4", data(d, 4)));
    CHECK(is("r 1FFFF 1", data(sram + 0x1FFFF, 1)));
    CHECK(is("r 1ff00 100", data(sram + 0x1FF00, 256)));                /* lower-case hex, the longest reply */
    CHECK(strlen(out) == 3u + 512u + 1u + 4u);
    CHECK(is("r 1FFFF 2", "err range"));
    CHECK(is("r 20000 1", "err range"));
    CHECK(is("r 0 101", "err range"));
    CHECK(is("r 0 0", "err range"));
    CHECK(is("r FFFFFFFF 2", "err range"));             /* no wrap */
    CHECK(is("r 0", "err ?"));
    CHECK(is("r 0 4 4", "err ?"));
    CHECK(is("r 0 G", "err ?"));
    CHECK(is("r 123456789 1", "err ?"));                /* more than 8 digits */
}

static void test_write(void)
{
    static const uint8_t d[3] = {0xA1, 0xB2, 0xC3};
    char line[400], *o;
    uint32_t i;
    sprintf(line, "w 10 A1B2C3 %04X", lsdj_crc16(d, 3));
    CHECK(is(line, "ok"));
    CHECK(sram[0x10] == 0xA1 && sram[0x11] == 0xB2 && sram[0x12] == 0xC3 && sram[0x13] == (uint8_t)(0x13 * 7));
    sprintf(line, "w 20 a1b2c3 %04x", lsdj_crc16(d, 3));               /* lower case */
    CHECK(is(line, "ok") && sram[0x22] == 0xC3);
    sprintf(line, "w 30 A1B2C3 %04X", (unsigned)(lsdj_crc16(d, 3) ^ 1u));
    CHECK(is(line, "err crc") && sram[0x30] == (uint8_t)(0x30 * 7));    /* nothing written */
    CHECK(is("w 30 A1B 0000", "err hex"));
    CHECK(is("w 30 A1ZZ 0000", "err hex"));
    CHECK(is("w 30  0000", "err ?"));                   /* no data */
    sprintf(line, "w 1FFFE A1B2C3 %04X", lsdj_crc16(d, 3));
    CHECK(is(line, "err range") && sram[0x1FFFE] == (uint8_t)(0x1FFFE * 7 + 0x1FF));
    o = line + sprintf(line, "w 1FFEC ");               /* 20 bytes at the very end: the longest line */
    for (i = 0; i < LSDJ_XFER_WR; i++)
        o += sprintf(o, "%02X", (unsigned)(i ^ 0x5Au));
    {
        uint8_t b[LSDJ_XFER_WR];
        for (i = 0; i < LSDJ_XFER_WR; i++)
            b[i] = (uint8_t)(i ^ 0x5Au);
        sprintf(o, " %04X", lsdj_crc16(b, LSDJ_XFER_WR));
        CHECK(strlen("ble ") + strlen(line) <= 63u);    /* fits the console's 64-character line */
        CHECK(is(line, "ok") && sram[0x1FFEC] == 0x5A && sram[0x1FFFF] == (uint8_t)(19 ^ 0x5A));
    }
    o = line + sprintf(line, "w 100 ");                 /* 21 bytes */
    for (i = 0; i < LSDJ_XFER_WR + 1u; i++)
        o += sprintf(o, "00");
    sprintf(o, " 0000");
    CHECK(is(line, "err range") && sram[0x100] == (uint8_t)(0x100 * 7 + 1));
    o = line + sprintf(line, "w 100 ");                 /* a very long line of hex: nothing overflows */
    for (i = 0; i < 150u; i++)
        o += sprintf(o, "FF");
    CHECK(is(line, "err range"));
}

static void test_bank(void)
{
    static const uint8_t d[4] = {0xE5, 0xE6, 0xFF, 0xFF};   /* bank 1: bytes 198, 199 stored, then the fill */
    static uint8_t whole[LSDJ_BANK];
    static const uint8_t ff = 0xFF;
    char want[16];
    uint32_t i;
    CHECK(is("b 1 C6 4", data(d, 4)));
    CHECK(is("b 2 3FFF 1", data(&ff, 1)));
    CHECK(is("b 3 0 1", "err range"));
    CHECK(is("b 0 3FFF 2", "err range"));
    CHECK(is("b 0 0 101", "err range"));
    CHECK(is("b 0 0 0", "err range"));
    memset(whole, 0xFF, sizeof whole);
    for (i = 0; i < 100u; i++)
        whole[i] = (uint8_t)i;
    sprintf(want, "ok %08X", ref_crc32(whole, sizeof whole));
    CHECK(is("bc 0", want));
    CHECK(is("bc 3", "err range"));
    CHECK(is("bc", "err ?"));
}

static void test_go_and_idle(void)
{
    CHECK(x.held == 1);
    now = 1000;
    CHECK(is("info", "ok v1 started 1 held 1 banks 03"));               /* a command while held restarts the timer */
    lsdj_xfer_tick(&x, 1000 + LSDJ_XFER_IDLE_MS - 1);
    CHECK(x.held == 1);
    lsdj_xfer_tick(&x, 1000 + LSDJ_XFER_IDLE_MS);
    CHECK(x.held == 0);                                 /* the host went away: the Game Boy runs again */
    now = 0;
    CHECK(is("hold", "ok") && x.held == 1);
    CHECK(is("go", "ok") && x.held == 0 && x.reset_req == 0);
    CHECK(is("hold", "ok"));
    CHECK(is("go reset", "ok") && x.held == 0 && x.reset_req == 1);
    CHECK(is("go again", "err ?"));
    x.reset_req = 0;
    x.started = 0;
    CHECK(is("go reset", "ok") && x.reset_req == 0);    /* nothing to restart before PLAY */
}

int main(void)
{
    vec = fopen("build/tests/lsdj_xfer_vectors.txt", "w");
    setup();
    test_crc();
    test_state();
    test_read();
    test_write();
    test_bank();
    test_go_and_idle();
    if (vec)
        fclose(vec);
    CHECK(vec != 0);
    printf(fails ? "lsdj_xfer_test: %d FAILED\n" : "lsdj_xfer_test: all passed\n", fails);
    return fails != 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `mkdir -p build/tests && cc -Wall -O1 -Ifirmware/src -o build/tests/lsdj_xfer_test tests/lsdj/lsdj_xfer_test.c`
Expected: FAIL, `fatal error: 'lsdj/lsdj_xfer.c' file not found`.

- [ ] **Step 3: Write the implementation**

Create `firmware/src/lsdj/lsdj_xfer.c`:

```c
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
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cc -Wall -O1 -Ifirmware/src -o build/tests/lsdj_xfer_test tests/lsdj/lsdj_xfer_test.c && build/tests/lsdj_xfer_test && head -4 build/tests/lsdj_xfer_vectors.txt`
Expected: `lsdj_xfer_test: all passed`, then `> info` / `< ok v1 started 0 held 0 banks 03` / `> hold` / `< err not started`. (Warnings about unused functions of `lsdj_gb.c` are expected.)

- [ ] **Step 5: Add the test to the suite**

In `tests/run_tests.sh`, after the line that starts `run "LSDj unit: ROM banks, keys, screen spans`, add:

```sh
run "LSDj transfer commands (the console's ble WORDS): hold, save RAM read / write, ROM banks" sh -c 'cc -Wall -O1 -Ifirmware/src -o build/tests/lsdj_xfer_test tests/lsdj/lsdj_xfer_test.c && build/tests/lsdj_xfer_test'
```

- [ ] **Step 6: Commit**

```bash
git status --short
git add firmware/src/lsdj/lsdj_xfer.c tests/lsdj/lsdj_xfer_test.c tests/run_tests.sh
git commit -m "lsdj: transfer commands (hold, save RAM read / write, ROM banks), host-tested"
```

---

### Task 2: The command in the firmware (the `ble` word, hold, restart)

**Files:**
- Modify: `firmware/src/ble/ble_core.h` (one declaration after `core_con_puts`)
- Modify: `firmware/src/core_ble_api.c` (one function after `core_con_puts`)
- Modify: `firmware/src/lsdj/lsdj.c`
- Modify: `tests/lsdj/lsdj_unit_test.c`

`firmware/src/console.c` is NOT edited. Measured on 2026-10-12: the compiler builds the whole console handler into `fm1_main`, which gate 2b compares, so any change to `console.c` (even the line length) fails the gate. The console's existing word `ble` already calls the unit's `ble_status()`; the unit reads the rest of that line through a new accessor. The console line stays 64 characters, which is why a write line carries at most 20 bytes.

**Interfaces:**
- Consumes: Task 1's `lsdj_xfer_t`, `lsdj_xfer_cmd`, `lsdj_xfer_tick`, `LSDJ_XFER_OUT`.
- Produces:
  - `const char *core_con_line(void);` (core: the console line being carried out, e.g. `"ble r 8000 100"`)
  - on the wire: the host sends `ble ARGS\r`; the console echoes it, then `\r\n`, one line `ok ...` or `err ...`, `\r\n`, `> `. A bare `ble` still prints the unit's status lines (no `ok` / `err` line), as does a firmware without this task.

- [ ] **Step 1: Write the failing test**

In `tests/lsdj/lsdj_unit_test.c` replace the line

```c
void core_con_puts(const char *s) { fputs(s, stdout); }
```

with

```c
static char con_log[4096];                    /* what the unit printed on the console */
static char con_line[64];                     /* the console line being carried out */
void core_con_puts(const char *s)
{
    fputs(s, stdout);
    if (strlen(con_log) + strlen(s) < sizeof con_log)
        strcat(con_log, s);
}
const char *core_con_line(void) { return con_line; }
```

Add these two functions just above `int main(void)` (the one inside `#if __has_include("lsdj_rom.h")`):

```c
static void console(const char *line)         /* as the core does: the line `ble ...`, then the unit's ble_status */
{
    strcpy(con_line, line);
    con_log[0] = 0;
    ble_status();
    con_line[0] = 0;
}

static void test_console(void)                /* after PLAY: `ble ...` holds and restarts the Game Boy */
{
    static const uint8_t d[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    char line[64];
    uint32_t i, f, t;
    console("ble");
    CHECK(strstr(con_log, "lsdj running") && !strstr(con_log, "ok"));   /* bare: the status, as before */
    console("  ble  ");
    CHECK(strstr(con_log, "lsdj running") != 0);
    console("ble info");
    CHECK(!strcmp(con_log, "ok v1 started 1 held 0 banks 40\r\n"));
    console("ble r 0 4");
    CHECK(!strcmp(con_log, "err hold\r\n"));          /* not while the Game Boy runs */
    console("ble hold");
    t = texts;
    f = st.frames;
    for (i = 0; i < 20; i++)
        pass();
    CHECK(st.frames == f);                            /* held: no Game Boy frame */
    CHECK(texts > t);                                 /* "USB" on the status line */
    sprintf(line, "ble w 1FF00 DEADBEEF %04X", lsdj_crc16(d, 4));   /* a free block at the end of the save */
    console(line);
    CHECK(!strcmp(con_log, "ok\r\n") && !memcmp(sram + 0x1FF00, d, 4));
    console("ble go reset");
    for (i = 0; i < 100; i++)
        pass();
    CHECK(xfer.reset_req == 0 && st.frames > f && emu.errors == 0);   /* LSDj started again and runs */
    CHECK(!memcmp(sram + 0x1FF00, d, 4));             /* the save RAM as it was written */
    console("ble hold");
    f = st.frames;
    for (i = 0; i < 240; i++)                         /* 4.8 s without a command */
        pass();
    CHECK(xfer.held == 1 && st.frames == f);
    for (i = 0; i < 20; i++)                          /* past 5 s: the host went away */
        pass();
    CHECK(xfer.held == 0 && st.frames > f);
}
```

In that `main`, add `test_console();` on the line before `ble_status();`.

- [ ] **Step 2: Run the test to verify it fails**

The unit test needs the private header. Make it once in this worktree (about a minute; it runs the ROM on the host and also builds `build/host/lsdj_host`, `build/lsdj/boot.sav` and `build/lsdj/demo.sav`):

```bash
mkdir -p build/lsdj build/tests
$HOME/Desktop/dev/fm1-lsdj/build/venv/bin/python tools/lsdj_rom.py $HOME/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb build/lsdj/lsdj_rom.h
cc -O1 -w -Ifirmware/src -Ifirmware/src/lsdj -Ibuild/lsdj -o build/tests/lsdj_unit_test tests/lsdj/lsdj_unit_test.c
```

Expected: FAIL to compile: `xfer` undeclared (in `test_console`).

- [ ] **Step 3: Add the core's accessor**

In `firmware/src/ble/ble_core.h`, after the line `void core_con_puts(const char *s);` add:

```c
/* the console line being carried out ("ble ...": the words after `ble` are the unit's); "" without a console */
const char *core_con_line(void);
```

In `firmware/src/core_ble_api.c`, after the closing brace of `core_con_puts`, add:

```c
const char *core_con_line(void)
{
#if FELUCCA_CDC
    return con.line;
#else
    return "";
#endif
}
```

- [ ] **Step 4: Add the unit's side**

In `firmware/src/lsdj/lsdj.c`:

After `#include "lsdj/lsdj_emu.c"` add:

```c
#include "lsdj/lsdj_xfer.c"
```

After the line `static uint32_t pad_map[8];                   /* button bit per Game Boy key (JOYPAD order), from KEYS */` add:

```c
static lsdj_xfer_t xfer;                      /* the console's `ble ...` (lsdj_xfer.c): the page moves songs with it */
static char xfer_out[LSDJ_XFER_OUT];
```

In the `st` struct, change the line

```c
    uint8_t pad, pad_shown;                   /* the Game Boy keys of the last frame; those on the status line */
```

to

```c
    uint8_t pad, pad_shown;                   /* the Game Boy keys of the last frame; those on the status line */
    uint8_t held_shown;                       /* "USB" is on the status line */
```

Replace the whole `keys_line` function (and the comment above it) with:

```c
/* right: USB while the host holds the Game Boy; else the Game Boy keys held, or with none the speed options (FAST /
 * SLOW ROM read, SKIP / FULL drawing) */
static void keys_line(void)
{
    char b[24];
    if (xfer.held) {
        put_s(b, "USB");
    } else if (st.pad == 0xFFu) {
        put_s(put_s(b, emu_mode & EMU_FAST_ROM ? "FAST " : "SLOW "), emu_mode & EMU_FRAME_SKIP ? "SKIP" : "FULL");
    } else {
        lsdj_pad_text(b, st.pad);
    }
    core_text(112, (int)STATUS_Y, 128, CORE_FONT_S, b, CORE_THEME, CORE_BG, 0);
    st.pad_shown = st.pad;
    st.held_shown = xfer.held;
}
```

Add this function just above `static void lsdj_step(void)`:

```c
static void lsdj_restart(void)                 /* `ble go reset`: the Game Boy from its reset, the save RAM kept */
{
    emu_init(lsdj_rom, LSDJ_ROM_BANKS, sram, fb);     /* (it ran before: the cartridge is LSDj) */
    emu.dirty = dirty;
    memset(shown, 0xFF, sizeof shown);        /* every line pair is sent again */
    memset(dirty, 1, sizeof dirty);
}
```

In `lsdj_step`, replace

```c
    if (st.pad != st.pad_shown)
        keys_line();
```

with

```c
    lsdj_xfer_tick(&xfer, core_ms());
    if (st.pad != st.pad_shown || xfer.held != st.held_shown)
        keys_line();
    if (xfer.held)
        return;                               /* the host reads or writes the save: no Game Boy frame */
    if (xfer.reset_req) {                     /* the save changed under LSDj: it starts again and reads it */
        xfer.reset_req = 0;
        lsdj_restart();
    }
```

In `lsdj_start`, after the line `run = lsdj_step;` add:

```c
    xfer.started = 1;
```

Add this function just above `void ble_status(void)`:

```c
static void lsdj_console(const char *args)     /* the words after `ble` on the console line: a transfer command */
{
    xfer.sram = sram;
    xfer.size = LSDJ_SRAM;
    xfer.rom = lsdj_rom;
    xfer.nbanks = LSDJ_ROM_BANKS;
    lsdj_xfer_cmd(&xfer, args, core_ms(), xfer_out);
    core_con_puts(xfer_out);
    core_con_puts("\r\n");
}
```

In `ble_status`, change its first lines from

```c
void ble_status(void)                          /* the console's `ble` */
{
    char b[96], *p = b;
```

to

```c
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
```

Also add to the comment at the top of `lsdj.c`, after the paragraph that ends `walks the direct calls from boot).`:

```c
 *
 * Songs move over the console: `ble WORDS` is a transfer command (lsdj_xfer.c; web/lsdj.html drives it). The host
 * holds the Game Boy between two frames, reads or writes the save RAM, and lets it run again. Nothing is written
 * to flash.
```

- [ ] **Step 5: Run the unit test to verify it passes**

Run: `cc -O1 -w -Ifirmware/src -Ifirmware/src/lsdj -Ibuild/lsdj -o build/tests/lsdj_unit_test tests/lsdj/lsdj_unit_test.c && build/tests/lsdj_unit_test | tail -3`
Expected: `lsdj_unit_test: all passed (N blits)`, no `FAIL` line.

- [ ] **Step 6: Build the firmware; the gates must pass**

Run (Docker running): `PYTHON=$HOME/Desktop/dev/fm1-lsdj/build/venv/bin/python FELUCCA_LSDJ=1 LSDJ_ROM=$HOME/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb ./build.sh 2>&1 | tail -12`
Expected, among the last lines (a trial of exactly these edits on 2026-10-12 gave image 565008 B):
- `ok    image NNNNNN B; ...` with the image under 585728 B and the pool under 344064 B
- `ok    update path: unchanged`
- `ok    no Game Boy reachable at boot (only through ble_key)`
- `package  .../build/lsdj-private.fwsc`

If `update path` reports a difference, do not touch `tests/ble/update_path.ref`. Run `git diff --stat lsdj-emu -- firmware/src/console.c firmware/src/usb.c firmware/src/core_main.c`: it must print nothing; undo whatever it shows.

- [ ] **Step 7: Run the whole suite**

Run: `LSDJ_ROM=$HOME/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb sh tests/run_tests.sh 2>&1 | tail -15`
Expected: the script's summary reports no failure; no line contains `FAIL`.

- [ ] **Step 8: Commit**

```bash
git status --short
git add firmware/src/ble/ble_core.h firmware/src/core_ble_api.c firmware/src/lsdj/lsdj.c tests/lsdj/lsdj_unit_test.c
git commit -m "lsdj: ble WORDS on the console is a transfer command: the Game Boy held for the host, restart after an upload, USB on the status line"
```

---

### Task 3: The formats (`web/lsdjfs.js`)

**Files:**
- Create: `web/lsdjfs.js`
- Create: `web/test_lsdj.mjs`
- Modify: `tests/run_tests.sh` (one line in the `node` block)

**Interfaces:**
- Consumes: nothing.
- Produces (all `export`):
  - constants `SAV_SIZE` (0x20000), `BLOCK` (0x200), `BANK` (0x4000), `SLOTS` (32), `NBLOCKS` (0xBF), `NAMES` (0x8000), `VERSIONS` (0x8100), `ACTIVE` (0x8140), `FAT` (0x8141), `BLOCKS` (0x8200)
  - `kitBank(kit) -> bank`, `bankKit(bank) -> kit`
  - `slotBlocks(sav, slot) -> number[]`, `freeBlocks(sav) -> number`
  - `nameText(sav, slot) -> string`, `songs(sav) -> [{slot, name, version, blocks}]`
  - `decodeSong(sav, slot) -> {song: Uint8Array(0x8000), src: Int32Array(0x8000)} | null`, `unpackSong(sav, slot) -> Uint8Array | null`
  - `usedKits(song) -> number[]`, `songKitBanks(sav, slot) -> number[]`
  - `buildSongFile(sav, slot, kits: Uint8Array[]) -> Uint8Array`, `songFileName(sav, slot) -> string`
  - `nextBlockPtr(block) -> number` (-1 at the end mark)
  - `parseSongFile(bytes) -> {name: Uint8Array(8), version, blocks: Uint8Array[], kits: Uint8Array[]}` (throws `Error`)
  - `importSong(sav, file, kitBanks = []) -> {sav: Uint8Array, slot}` (a changed copy; throws `Error`)
  - `removeSong(sav, slot) -> Uint8Array` (a changed copy)
  - `crc32(bytes) -> number`
- Rules for both web modules: every `import` is one line (`web/make_site.py` strips imports with a one-line pattern); top-level names must not repeat between `lsdjfs.js` and `lsdjlink.js` (the site inlines both into one script).

- [ ] **Step 1: Write the failing test**

Create `web/test_lsdj.mjs`:

```js
// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// Node checks of the LSDj song page's JS (no browser, no hardware, no LSDj data). Run from the repo root:
//   node web/test_lsdj.mjs
// - lsdjfs.js: the save's file system and song files, on songs made here
// - lsdjlink.js: the console protocol and the page's flows against a simulated FM-1, which must answer
//   build/tests/lsdj_xfer_vectors.txt (written by tests/lsdj/lsdj_xfer_test.c) as the firmware does
// - lsdj.html with both modules inlined (as web/make_site.py does) is one script that compiles

import * as fs from "./lsdjfs.js";

let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(72)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const throws = (fn, re) => { try { fn(); } catch (e) { return re.test(e.message); } return false; };

/* ---- a save and songs made here ---- */
function emptySav() {
    const sav = new Uint8Array(fs.SAV_SIZE);
    sav.fill(0xFF, fs.ACTIVE, fs.BLOCKS);                // no active slot, every block free
    sav[0x813E] = 0x6A; sav[0x813F] = 0x6B;              // "jk": LSDj's mark of an initialised file system
    return sav;
}
function testSong(seed) {                                // 32 KiB with escapes, runs and two kit instruments
    const s = new Uint8Array(0x8000);
    for (let i = 0; i < 0x1000; i++) s[i] = (i * 13 + seed) & 0xFF;      // every byte value, 0xC0 and 0xE0 too
    s.fill(0xC0, 0x1000, 0x1040);                        // a run of an escape byte
    s.fill(0xE0, 0x1040, 0x1043);                        // three: too short for a run here
    for (let i = 0; i < 0x40; i++) s.set([0xA8, 0, 0, 0xFF, 0, 0, 3, 0, 0, 0xD0, 0, 0, 0, 0xF3, 0, 0], 0x3080 + 16 * i);
    s.set([2, 0x11, 0x83, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x05, 0x88], 0x3080);          // kits 3 (bit 7 set), 5
    s.set([2, 0x12, 0x03, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x03, 0x89], 0x3080 + 16 * 7); // kits 3, 3
    return s;
}
function packSong(song) {                                // tokens: literals with escapes, runs of 4 or more
    const t = [];                                        // (no run of 0xC0: C0 C0 n would read as an escaped C0)
    for (let i = 0; i < song.length;) {
        let n = 1;
        while (i + n < song.length && song[i + n] === song[i] && n < 255) n++;
        if (n >= 4 && song[i] !== 0xC0) { t.push([0xC0, song[i], n]); i += n; }
        else { t.push(song[i] === 0xC0 ? [0xC0, 0xC0] : song[i] === 0xE0 ? [0xE0, 0xE0] : [song[i]]); i++; }
    }
    return t;
}
function addPacked(sav, slot, song, name, version) {     // the song into free blocks, as LSDj lays it out
    const blocks = [[]];
    for (const tok of packSong(song)) {
        if (blocks.at(-1).length + tok.length > fs.BLOCK - 2) blocks.push([]);
        blocks.at(-1).push(...tok);
    }
    const ids = [];
    for (let i = 0; i < fs.NBLOCKS && ids.length < blocks.length; i++) if (sav[fs.FAT + i] >= fs.SLOTS) ids.push(i);
    blocks.forEach((b, k) => {
        b.push(0xE0, k + 1 < blocks.length ? ids[k + 1] + 1 : 0xFF);
        sav[fs.FAT + ids[k]] = slot;
        sav.fill(0, fs.BLOCKS + fs.BLOCK * ids[k], fs.BLOCKS + fs.BLOCK * (ids[k] + 1));
        sav.set(b, fs.BLOCKS + fs.BLOCK * ids[k]);
    });
    sav.fill(0, fs.NAMES + 8 * slot, fs.NAMES + 8 * slot + 8);
    sav.set([...name].map(c => c.charCodeAt(0)), fs.NAMES + 8 * slot);
    sav[fs.VERSIONS + slot] = version;
    return ids;
}
const kitBankData = n => { const b = new Uint8Array(fs.BANK); b[0] = 0x60; b[1] = 0x40; b.fill(n, 2); return b; };

/* ---- lsdjfs ---- */
{
    ok(fs.kitBank(0) === 8 && fs.kitBank(18) === 26 && fs.kitBank(19) === 32 && fs.kitBank(50) === 63, "kit number -> ROM bank");
    ok([8, 26, 32, 33, 63].every(b => fs.kitBank(fs.bankKit(b)) === b) && fs.bankKit(32) === 19, "ROM bank -> kit number");
    ok(fs.crc32(new TextEncoder().encode("123456789")) === 0xCBF43926, "CRC-32 check value");

    const sav = emptySav(), songA = testSong(1), songB = testSong(9);
    ok(fs.songs(sav).length === 0 && fs.freeBlocks(sav) === 0xBF, "an empty save: no songs, 191 free blocks");
    const idsA = addPacked(sav, 0, songA, "ABC 9", 0x12), idsB = addPacked(sav, 5, songB, "ZZTOP123", 4);
    ok(idsA.length > 1 && idsB[0] === idsA.length, "the test songs span several blocks");
    const list = fs.songs(sav);
    ok(list.length === 2 && list[0].slot === 0 && list[0].name === "ABC 9" && list[0].version === 0x12 &&
       list[0].blocks === idsA.length && list[1].slot === 5 && list[1].name === "ZZTOP123", "the song list");
    sav[fs.NAMES + 2] = 0x21;                            // a character outside A-Z 0-9 reads as a space
    ok(fs.nameText(sav, 0) === "AB  9", "names: only A-Z and 0-9");
    sav[fs.NAMES + 2] = 0x43;
    ok(fs.freeBlocks(sav) === 0xBF - idsA.length - idsB.length, "free blocks");

    const d = fs.decodeSong(sav, 0);
    ok(d && eq(d.song, songA) && eq(fs.unpackSong(sav, 5), songB), "songs unpack to their 32 KiB");
    ok(d.src[0x3082] >= fs.BLOCKS && sav[d.src[0x3082]] === 0x83 && d.src[0x1000] === -1, "each literal byte knows its place in the save");
    ok(fs.unpackSong(sav, 3) === null, "an empty slot unpacks to nothing");
    ok(eq(fs.usedKits(songA), [3, 5]) && eq(fs.songKitBanks(sav, 0), [11, 13]), "the kits a song uses, as ROM banks");

    const file = fs.buildSongFile(sav, 0, [kitBankData(1), kitBankData(2)]);
    ok(file.length === 9 + fs.BLOCK * idsA.length + 2 * fs.BANK && file[8] === 0x12 && file[0] === 0x41, "a song file: name, version, blocks, kits");
    ok(fs.songFileName(sav, 0) === "ABC_9-v12.lsdprj", "the file name carries the song's version");
    const parsed = fs.parseSongFile(file);
    ok(parsed.blocks.length === idsA.length && parsed.kits.length === 2 && parsed.kits[1][2] === 2 && parsed.version === 0x12, "a song file parses back");
    ok(fs.parseSongFile(file.subarray(0, 9 + fs.BLOCK * idsA.length)).kits.length === 0, "an .lsdsng (no kits) parses");

    /* into another save whose first slot and first blocks are taken */
    const other = emptySav();
    addPacked(other, 0, songB, "TAKEN", 1);
    const before = other.slice();
    const r = fs.importSong(other, parsed, [40, 9]);     // the kits are in banks 40 and 9 over there
    ok(eq(other, before), "an import changes a copy, not the save it was given");
    ok(r.slot === 1 && fs.songs(r.sav).length === 2 && fs.songs(r.sav)[1].name === "ABC 9", "the song takes the first free slot");
    const got = fs.unpackSong(r.sav, 1), want = songA.slice();
    want[0x3082] = 0x80 | fs.bankKit(40); want[0x3089] = fs.bankKit(9);
    want[0x3080 + 16 * 7 + 2] = fs.bankKit(40); want[0x3080 + 16 * 7 + 9] = fs.bankKit(40);
    ok(got && eq(got, want), "the song arrives whole, its kits renumbered, the other bits kept");
    ok(eq(fs.unpackSong(r.sav, 0), songB), "the song that was there is untouched");
    const mine = fs.slotBlocks(r.sav, 1);
    ok(mine[0] === fs.slotBlocks(other, 0).length && mine.every((b, k) => k + 1 === mine.length ||
       r.sav[fs.BLOCKS + fs.BLOCK * b + fs.nextBlockPtr(r.sav.subarray(fs.BLOCKS + fs.BLOCK * b, fs.BLOCKS + fs.BLOCK * (b + 1)))] === mine[k + 1] + 1),
       "each block's jump names the next block, counted from 1");
    ok(eq(fs.songKitBanks(r.sav, 1), [9, 40]), "over there the song names banks 9 and 40");
    const again = fs.importSong(r.sav, fs.parseSongFile(fs.buildSongFile(r.sav, 1, [kitBankData(2), kitBankData(1)])), [9, 40]);
    ok(eq(fs.unpackSong(again.sav, 2), want), "exported and imported again: the same song");
    ok(eq(fs.unpackSong(fs.importSong(other, fs.parseSongFile(file.subarray(0, 9 + fs.BLOCK * idsA.length))).sav, 1), songA), "an .lsdsng keeps its kit numbers");

    /* refusals */
    ok(throws(() => fs.parseSongFile(file.subarray(0, 300)), /too short/), "a file shorter than a block is refused");
    ok(throws(() => fs.parseSongFile(file.subarray(0, 9 + fs.BLOCK)), /ends inside/), "a file cut inside its blocks is refused");
    ok(throws(() => fs.parseSongFile(file.subarray(0, file.length - 1)), /whole 16 KiB/), "kit data that is not whole banks is refused");
    const noEnd = file.slice(0, 9 + fs.BLOCK); noEnd.fill(0x11, 9);
    ok(throws(() => fs.parseSongFile(noEnd), /corrupted/), "a block without a jump or an end is refused");
    const short = fs.parseSongFile(file); short.blocks.pop();
    ok(throws(() => fs.importSong(other, short, [40, 9]), /no end/), "a song whose last block has no end mark is refused");
    const holed = fs.parseSongFile(file); holed.blocks.splice(1, 1);
    ok(throws(() => fs.importSong(other, holed, [40, 9]), /does not unpack/), "a song that does not unpack whole is refused");
    ok(throws(() => fs.importSong(other, parsed, [40]), /kits/), "a kit list of the wrong length is refused");
    const full = emptySav(); for (let s = 0; s < 32; s++) full[fs.FAT + s] = s;
    ok(throws(() => fs.importSong(full, parsed, [40, 9]), /no free song slot/), "32 songs: refused");
    const tight = emptySav(); tight.fill(0, fs.FAT + 1, fs.FAT + fs.NBLOCKS);
    ok(throws(() => fs.importSong(tight, parsed, [40, 9]), /not enough room/), "too few free blocks: refused");

    /* Review Focus 5: a renumbered kit byte never becomes an escape (0xC0 / 0xE0) */
    const esc = testSong(1); esc[0x3082] = 0xC3;         // kit 3 with both top bits set
    const sav2 = emptySav(); addPacked(sav2, 0, esc, "ESC", 1);
    const f2 = fs.parseSongFile(fs.buildSongFile(sav2, 0, [kitBankData(1), kitBankData(2)]));
    ok(throws(() => fs.importSong(emptySav(), f2, [8, 9]), /cannot be renumbered/), "a renumbered kit byte never becomes an escape (kit 0 -> 0xC0)");
    ok(throws(() => fs.importSong(emptySav(), f2, [45, 9]), /cannot be renumbered/), "... nor 0xE0 (kit 32)");
    ok(fs.unpackSong(fs.importSong(emptySav(), f2, [11, 13]).sav, 0)[0x3082] === 0xC3, "the same kit numbers need no change");

    const gone = fs.removeSong(r.sav, 0);
    ok(fs.songs(gone).length === 1 && fs.songs(gone)[0].slot === 1 && fs.freeBlocks(gone) === 0xBF - mine.length && eq(fs.unpackSong(gone, 1), want), "a removed song frees its slot and blocks");
    const act = r.sav.slice(); act[fs.ACTIVE] = 0;
    ok(fs.removeSong(act, 0)[fs.ACTIVE] === 0xFF && fs.removeSong(act, 1)[fs.ACTIVE] === 0, "removing the active song clears the mark");
}

console.log(failed ? `test_lsdj: ${failed} FAILED` : "test_lsdj: all passed");
process.exit(failed ? 1 : 0);
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `node web/test_lsdj.mjs`
Expected: FAIL, `Cannot find module '.../web/lsdjfs.js'`.

- [ ] **Step 3: Write the implementation**

Create `web/lsdjfs.js`:

```js
// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// LSDj's save file system and song files, with no browser and no device. The rules are LSDPatcher's (Johan
// Kotlinski, MIT: LSDSavFile.java), written anew.
//   the save (128 KiB): song names at 0x8000 (32 x 8), versions at 0x8100, the active slot at 0x8140, the block
//   table at 0x8141 (191 blocks: the slot that owns each, 0xFF free), blocks of 512 B from 0x8200
//   a song file (.lsdsng; .lsdprj with kits): 8 name bytes, 1 version byte, the song's blocks in order, then one
//   16 KiB ROM bank per kit the song uses, in rising kit order
//   packing: C0 C0 = C0, C0 b n = n times b, E0 E0 = E0, E0 F0 n = n default waves, E0 F1 n = n default
//   instruments, E0 FF = the end, E0 k = go on in block k (counted from 1: 0x8000 + 0x200 * k)
export const SAV_SIZE = 0x20000, BLOCK = 0x200, BANK = 0x4000, SLOTS = 32, NBLOCKS = 0xBF;
export const NAMES = 0x8000, VERSIONS = 0x8100, ACTIVE = 0x8140, FAT = 0x8141, BLOCKS = 0x8200;
const FS_BASE = 0x8000, SONG_SIZE = 0x8000, INSTR = 0x3080, NINSTR = 0x40;
const DEF_WAVE = [0x8E, 0xCD, 0xCC, 0xBB, 0xAA, 0xA9, 0x99, 0x88, 0x87, 0x76, 0x66, 0x55, 0x54, 0x43, 0x32, 0x31];
const DEF_INSTR = [0xA8, 0, 0, 0xFF, 0, 0, 3, 0, 0, 0xD0, 0, 0, 0, 0xF3, 0, 0];

/* kits live in ROM banks 8..26 and 32..63 */
export const kitBank = kit => (kit + 8 > 26 ? kit + 13 : kit + 8);
export const bankKit = bank => (bank > 26 ? bank - 13 : bank - 8);

export function slotBlocks(sav, slot) {
    const out = [];
    for (let i = 0; i < NBLOCKS; i++) if (sav[FAT + i] === slot) out.push(i);
    return out;
}

export function freeBlocks(sav) {
    let n = 0;
    for (let i = 0; i < NBLOCKS; i++) if (sav[FAT + i] >= SLOTS) n++;
    return n;
}

export function nameText(sav, slot) {
    let s = "";
    for (let i = 0; i < 8; i++) {
        const c = sav[NAMES + 8 * slot + i];
        if (!c) break;
        s += (c >= 65 && c <= 90) || (c >= 48 && c <= 57) ? String.fromCharCode(c) : " ";
    }
    return s;
}

export function songs(sav) {
    const out = [];
    for (let slot = 0; slot < SLOTS; slot++) {
        const blocks = slotBlocks(sav, slot).length;
        if (blocks) out.push({ slot, name: nameText(sav, slot), version: sav[VERSIONS + slot], blocks });
    }
    return out;
}

/* a slot's song: { song: its 32 KiB, src: for each song byte the save address it was copied from, -1 if it came
   from a run or a default }; null for an empty slot or a damaged song */
export function decodeSong(sav, slot) {
    const first = slotBlocks(sav, slot)[0];
    if (first === undefined) return null;
    const song = new Uint8Array(SONG_SIZE), src = new Int32Array(SONG_SIZE).fill(-1);
    let p = BLOCKS + BLOCK * first, o = 0, jumps = 0;
    const put = (v, from) => { if (o >= SONG_SIZE) return false; song[o] = v; src[o++] = from; return true; };
    const fill = (pat, count) => {
        for (let k = 0; k < count * pat.length; k++) if (!put(pat[k % pat.length], -1)) return false;
        return true;
    };
    for (;;) {
        if (p + 2 >= sav.length) return null;
        const c = sav[p], n = sav[p + 1];
        if (c === 0xC0) {
            if (n === 0xC0) { if (!put(0xC0, -1)) return null; p += 2; }
            else { if (!fill([n], sav[p + 2])) return null; p += 3; }
        } else if (c === 0xE0) {
            if (n === 0xE0) { if (!put(0xE0, -1)) return null; p += 2; }
            else if (n === 0xFF) return o === SONG_SIZE ? { song, src } : null;
            else if (n === 0xF0) { if (!fill(DEF_WAVE, sav[p + 2])) return null; p += 3; }
            else if (n === 0xF1) { if (!fill(DEF_INSTR, sav[p + 2])) return null; p += 3; }
            else {
                if (n < 1 || n > NBLOCKS || ++jumps > NBLOCKS) return null;
                p = FS_BASE + BLOCK * n;
            }
        } else {
            if (!put(c, p)) return null;
            p += 1;
        }
    }
}

export function unpackSong(sav, slot) {
    const d = decodeSong(sav, slot);
    return d ? d.song : null;
}

/* kit numbers of the song's kit instruments (type 2: bytes 2 and 9, low 6 bits), rising */
export function usedKits(song) {
    const kits = new Set();
    for (let i = 0; i < NINSTR; i++) {
        const p = INSTR + 16 * i;
        if (song[p] !== 2) continue;
        kits.add(song[p + 2] & 0x3F);
        kits.add(song[p + 9] & 0x3F);
    }
    return [...kits].sort((a, b) => a - b);
}

/* the ROM banks a slot's song needs with it, in the order a song file carries them */
export function songKitBanks(sav, slot) {
    const song = unpackSong(sav, slot);
    if (!song) throw new Error("the song is damaged");
    return usedKits(song).map(kitBank);
}

export function buildSongFile(sav, slot, kits) {
    const blocks = slotBlocks(sav, slot);
    if (!blocks.length) throw new Error("the slot is empty");
    const out = new Uint8Array(9 + BLOCK * blocks.length + BANK * kits.length);
    out.set(sav.subarray(NAMES + 8 * slot, NAMES + 8 * slot + 8), 0);
    out[8] = sav[VERSIONS + slot];
    blocks.forEach((b, i) => out.set(sav.subarray(BLOCKS + BLOCK * b, BLOCKS + BLOCK * (b + 1)), 9 + BLOCK * i));
    kits.forEach((k, i) => {
        if (k.length !== BANK) throw new Error("a kit bank is not 16 KiB");
        out.set(k, 9 + BLOCK * blocks.length + BANK * i);
    });
    return out;
}

export function songFileName(sav, slot) {
    const name = nameText(sav, slot).trim().replace(/ /g, "_") || "SONG";
    return `${name}-v${sav[VERSIONS + slot].toString(16).toUpperCase().padStart(2, "0")}.lsdprj`;
}

/* in a block: the offset of the byte that names the next block, or -1 at the end mark */
export function nextBlockPtr(b) {
    for (let i = 0; i + 1 < BLOCK; i++) {
        if (b[i] === 0xC0) {
            i++;
            if (b[i] !== 0xC0) i++;
        } else if (b[i] === 0xE0) {
            const n = b[i + 1];
            if (n === 0xE0) i++;
            else if (n === 0xFF) return -1;
            else if (n === 0xF0 || n === 0xF1) i += 2;
            else return i + 1;
        }
    }
    throw new Error("song corrupted: a block without a jump or an end");
}

export function parseSongFile(bytes) {
    if (bytes.length < 9 + BLOCK) throw new Error("not a song file: too short");
    const blocks = [];
    let p = 9;
    for (;;) {
        if (p + BLOCK > bytes.length) throw new Error("song corrupted: the file ends inside its blocks");
        if (blocks.length === NBLOCKS) throw new Error("song corrupted: more blocks than a save holds");
        const b = bytes.slice(p, p + BLOCK);
        blocks.push(b);
        p += BLOCK;
        if (nextBlockPtr(b) < 0) break;
    }
    if ((bytes.length - p) % BANK) throw new Error("not a song file: the kit data is not whole 16 KiB banks");
    const kits = [];
    for (; p < bytes.length; p += BANK) kits.push(bytes.slice(p, p + BANK));
    return { name: bytes.slice(0, 8), version: bytes[8], blocks, kits };
}

/* a copy of the save with the song in its first free slot. kitBanks[i]: the ROM bank that holds file.kits[i] where
   the save lives (an .lsdsng brings none and keeps its kit numbers). Throws, changing nothing, when the song does
   not fit, is damaged or cannot be renumbered. */
export function importSong(sav, file, kitBanks = []) {
    const out = sav.slice();
    let slot = 0;
    while (slot < SLOTS && slotBlocks(out, slot).length) slot++;
    if (slot === SLOTS) throw new Error("no free song slot: all 32 are used");
    if (file.blocks.length > freeBlocks(out))
        throw new Error(`not enough room: the song needs ${file.blocks.length} blocks, ${freeBlocks(out)} are free`);
    out.fill(0, NAMES + 8 * slot, NAMES + 8 * slot + 8);
    out.set(file.name.subarray(0, 8), NAMES + 8 * slot);
    out[VERSIONS + slot] = file.version;
    let prev = -1;
    for (const b of file.blocks) {
        let id = 0;
        while (out[FAT + id] < SLOTS) id++;
        if (prev >= 0) out[prev] = id + 1;
        out[FAT + id] = slot;
        out.set(b, BLOCKS + BLOCK * id);
        const j = nextBlockPtr(b);
        prev = j < 0 ? -1 : BLOCKS + BLOCK * id + j;
    }
    if (prev >= 0) throw new Error("song corrupted: its last block has no end mark");
    const d = decodeSong(out, slot);
    if (!d) throw new Error("song corrupted: it does not unpack to a whole song");
    if (!file.kits.length) return { sav: out, slot };
    const old = usedKits(d.song);
    if (old.length !== file.kits.length || kitBanks.length !== file.kits.length)
        throw new Error(`the file carries ${file.kits.length} kits, the song uses ${old.length}`);
    const map = new Map(old.map((k, i) => [k, bankKit(kitBanks[i])]));
    for (let i = 0; i < NINSTR; i++) {
        const p = INSTR + 16 * i;
        if (d.song[p] !== 2) continue;
        for (const q of [p + 2, p + 9]) {
            const v = d.song[q], k = map.get(v & 0x3F), nv = (v & 0xC0) | k;
            if (nv === v) continue;
            if (d.src[q] < 0 || nv === 0xC0 || nv === 0xE0)   // inside a run, or the new byte would be an escape
                throw new Error("this song's kit numbers cannot be renumbered for this FM-1");
            out[d.src[q]] = nv;
        }
    }
    return { sav: out, slot };
}

export function removeSong(sav, slot) {
    const out = sav.slice();
    for (let i = 0; i < NBLOCKS; i++) if (out[FAT + i] === slot) out[FAT + i] = 0xFF;
    out[NAMES + 8 * slot] = 0;
    out[VERSIONS + slot] = 0;
    if (out[ACTIVE] === slot) out[ACTIVE] = 0xFF;
    return out;
}

let crcTable = null;
export function crc32(bytes) {
    if (!crcTable) {
        crcTable = new Uint32Array(256);
        for (let n = 0; n < 256; n++) {
            let c = n;
            for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
            crcTable[n] = c >>> 0;
        }
    }
    let c = 0xFFFFFFFF;
    for (const b of bytes) c = crcTable[(c ^ b) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `node web/test_lsdj.mjs | tail -5`
Expected: every line ends `ok`, the last line is `test_lsdj: all passed`.

- [ ] **Step 5: Add the test to the suite**

In `tests/run_tests.sh`, inside the `if command -v node` block, after the `test_backup.mjs` line, add:

```sh
    run "LSDj songs: the save's file system, song files, the console link, the page's flows" node web/test_lsdj.mjs
```

- [ ] **Step 6: Commit**

```bash
git status --short
git add web/lsdjfs.js web/test_lsdj.mjs tests/run_tests.sh
git commit -m "lsdj: the save's file system and song files (.lsdsng / .lsdprj) in JavaScript, node-tested"
```

---

### Task 4: The link and the page's flows (`web/lsdjlink.js`)

**Files:**
- Create: `web/lsdjlink.js`
- Modify: `web/test_lsdj.mjs` (a second block before the final two lines)

**Interfaces:**
- Consumes: Task 3's exports; Task 1's wire format and vectors file; Task 2's console framing (`ble WORDS\r` echoed, `\r\n`, reply, `\r\n`, `> `).
- Produces (all `export`):
  - `crc16(bytes) -> number`
  - `class Link { constructor(send, timeoutMs = 2000); feed(bytes); sync(); cmd(line) -> Promise<string>; info() -> {version, started, held, banks}; hold(); go(reset = false); read(addr, len, progress); write(addr, bytes, progress); readBank(bank, progress); bankCrc(bank) }` where `send(Uint8Array) -> Promise` and `progress(done, total)`
  - `listSongs(link) -> Promise<{sav, list, free}>` (`sav`: a 128 KiB image with only the file-system header read)
  - `downloadSong(link, slot, progress) -> Promise<{name, bytes}>`
  - `uploadSong(link, bytes, bankCrcs, progress) -> Promise<slot>`
  - `deleteSong(link, slot) -> Promise`
  - `downloadSav(link, progress) -> Promise<Uint8Array>`, `uploadSav(link, bytes, progress) -> Promise`
  - `bankCrcs(link, n) -> Promise<number[]>`
- Errors are `Error` objects whose `message` is a sentence for the person; a device error starts `FM-1: `.

- [ ] **Step 1: Write the failing test**

In `web/test_lsdj.mjs`, add to the imports at the top:

```js
import { existsSync, readFileSync } from "node:fs";
import vm from "node:vm";
import * as ln from "./lsdjlink.js";
```

and insert this block just before the line `console.log(failed ? ...`:

```js
/* ---- a simulated FM-1: the console, whose `ble WORDS` are the commands of firmware/src/lsdj/lsdj_xfer.c ---- */
class Sim {
    constructor(sram, banks) {                          // banks: the stored bytes of each (the rest reads FF)
        Object.assign(this, { sram, banks, started: false, held: false, resets: 0, line: "", log: [], link: null,
                              mute: false, corrupt: 0, failWrite: -1, failFrom: Infinity, writes: 0, hasLsdj: true, onHold: null });
    }
    bankByte(b, i) { return i < this.banks[b].length ? this.banks[b][i] : 0xFF; }
    data(d) { return `ok ${ln.hexBytes(d)} ${ln.crc16(d).toString(16).toUpperCase().padStart(4, "0")}`; }
    exec(text) {
        const w = text.trim().split(/ +/), num = s => (/^[0-9A-Fa-f]{1,8}$/.test(s || "") ? parseInt(s, 16) : NaN);
        const [c, ...a] = w;
        if (c === "info" && !a.length) return `ok v1 started ${+this.started} held ${+this.held} banks ${this.banks.length.toString(16).toUpperCase().padStart(2, "0")}`;
        if (c === "hold" && !a.length) {
            if (!this.started) return "err not started";
            this.held = true; this.onHold?.(); return "ok";
        }
        if (c === "go" && (!a.length || (a.length === 1 && a[0] === "reset"))) {
            this.held = false; if (a.length && this.started) this.resets++; return "ok";
        }
        if (c === "r" && a.length === 2 && !isNaN(num(a[0])) && !isNaN(num(a[1]))) {
            const [at, n] = a.map(num);
            if (!this.held) return "err hold";
            if (!n || n > 256 || at + n > this.sram.length) return "err range";
            return this.data(this.sram.subarray(at, at + n));
        }
        if (c === "w" && a.length >= 1 && !isNaN(num(a[0]))) {
            const h = a[1] || "";
            if (/[^0-9A-Fa-f]/.test(h) || h.length % 2) return "err hex";
            if (h.length > 40) return "err range";
            if (!h.length || a.length !== 3 || isNaN(num(a[2]))) return "err ?";
            const d = ln.unhex(h), at = num(a[0]);
            if (!this.held) return "err hold";
            if (at + d.length > this.sram.length) return "err range";
            if (num(a[2]) !== ln.crc16(d)) return "err crc";
            if (this.writes === this.failWrite || this.writes >= this.failFrom) { this.writes++; return "err crc"; }   // one bad line / a dead link
            this.writes++;
            this.sram.set(d, at); this.log.push(at); return "ok";
        }
        if (c === "b" && a.length === 3 && a.every(s => !isNaN(num(s)))) {
            const [b, off, n] = a.map(num);
            if (b >= this.banks.length || !n || n > 256 || off + n > 0x4000) return "err range";
            return this.data(Uint8Array.from({ length: n }, (_, i) => this.bankByte(b, off + i)));
        }
        if (c === "bc" && a.length === 1 && !isNaN(num(a[0]))) {
            const b = num(a[0]);
            if (b >= this.banks.length) return "err range";
            return "ok " + fs.crc32(Uint8Array.from({ length: 0x4000 }, (_, i) => this.bankByte(b, i))).toString(16).toUpperCase().padStart(8, "0");
        }
        return "err ?";
    }
    send(bytes) {                                       // the console: echo, and on CR the reply and the prompt
        let outp = "";
        for (const ch of bytes) {
            if (ch !== 13) { this.line += String.fromCharCode(ch); outp += String.fromCharCode(ch); continue; }
            const l = this.line; this.line = "";
            let reply = !l ? "" : !/^ble( |$)/.test(l) ? "? (help)\r\n" :           // an older firmware: `ble` is only the status
                        this.hasLsdj && l.trim() !== "ble" ? this.exec(l.slice(3)) + "\r\n" : "lsdj running error 0 emu_errors 0\r\n";
            if (this.corrupt > 0 && /^ok [0-9A-F]{2}/.test(reply)) { this.corrupt--; reply = reply.replace(/^ok ../, "ok ZZ"); }
            outp += "\r\n" + reply + "> ";
        }
        if (this.mute) return Promise.resolve();
        for (let i = 0; i < outp.length; i += 7)        // in small pieces, later, as USB delivers them
            queueMicrotask(() => this.link.feed(Uint8Array.from(outp.slice(i, i + 7), c => c.charCodeAt(0))));
        return Promise.resolve();
    }
}
function simWith(sav, banks = [[0x11], [0x22]]) {
    const sim = new Sim(sav.slice(), banks.map(b => Uint8Array.from(b)));
    sim.started = true;
    sim.link = new ln.Link(b => sim.send(b), 150);
    return sim;
}
const rejects = async (p, re) => { try { await p; } catch (e) { return re.test(e.message); } return false; };

/* the firmware's own answers (tests/lsdj/lsdj_xfer_test.c wrote them): the simulated device must give the same */
{
    const V = new URL("../build/tests/lsdj_xfer_vectors.txt", import.meta.url).pathname;
    if (!existsSync(V)) console.log("== skip: no build/tests/lsdj_xfer_vectors.txt (run tests/lsdj/lsdj_xfer_test first)");
    else {
        const sram = Uint8Array.from({ length: 0x20000 }, (_, i) => (i * 7 + (i >> 8)) & 0xFF);
        const sim = new Sim(sram, [0, 1, 2].map(b => Uint8Array.from({ length: 100 * (b + 1) }, (_, j) => (b * 31 + j) & 0xFF)));
        let bad = 0, n = 0, cmd = null;
        for (const l of readFileSync(V, "utf8").split("\n")) {
            if (l === "! started") sim.started = true;
            else if (l.startsWith("> ")) cmd = l.slice(2);
            else if (l.startsWith("< ")) { n++; if (sim.exec(cmd) !== l.slice(2)) { bad++; console.log(`   ${cmd} -> ${sim.exec(cmd)}, the firmware: ${l.slice(2)}`); } }
        }
        ok(n > 40 && bad === 0, `the simulated FM-1 answers ${n} commands as the firmware does`);
    }
}

/* ---- the link ---- */
await (async () => {
    ok(ln.crc16(new TextEncoder().encode("123456789")) === 0x31C3, "CRC-16 check value");
    const base = emptySav(); addPacked(base, 2, testSong(4), "DEVSONG", 7);
    let sim = simWith(base);
    await sim.link.sync();
    const info = await sim.link.info();
    ok(info.version === 1 && info.started && !info.held && info.banks === 2, "info");
    ok(await rejects(sim.link.read(0, 4), /hold/), "a read without hold is refused by the device");
    await sim.link.hold();
    ok(eq(await sim.link.read(0x8000, 1000), base.subarray(0x8000, 0x8000 + 1000)), "1000 bytes read in pieces of 256");
    const blob = Uint8Array.from({ length: 200 }, (_, i) => 255 - i);
    await sim.link.write(0x100, blob);
    ok(eq(sim.sram.subarray(0x100, 0x100 + 200), blob) && sim.log.length === 10 && sim.log[1] === 0x114 && sim.log[9] === 0x1B4, "200 bytes written in pieces of 20");
    sim.failWrite = sim.writes;
    await sim.link.write(0x200, blob.subarray(0, 8));
    ok(eq(sim.sram.subarray(0x200, 0x208), blob.subarray(0, 8)), "a write refused once is sent again");
    sim.corrupt = 2;
    ok(eq(await sim.link.read(0x8000, 16), base.subarray(0x8000, 0x8010)), "a damaged reply is asked for again");
    sim.corrupt = 3;
    ok(await rejects(sim.link.read(0x8000, 16), /damaged/), "three damaged replies: the read stops");
    ok((await sim.link.bankCrc(1)) === fs.crc32(Uint8Array.from({ length: 0x4000 }, (_, i) => (i ? 0xFF : 0x22))), "a bank's CRC-32");
    const [r1, r2] = await Promise.all([sim.link.read(0x8000, 8), sim.link.read(0x8100, 8)]);
    ok(eq(r1, base.subarray(0x8000, 0x8008)) && eq(r2, base.subarray(0x8100, 0x8108)), "two commands at once are answered in turn");
    sim.mute = true;
    ok(await rejects(sim.link.info(), /does not answer/), "silence: the FM-1 does not answer");
    sim = simWith(base); sim.hasLsdj = false;
    ok(await rejects(sim.link.info(), /no transfer command/), "a firmware without the command is named");

    /* ---- the page's flows ---- */
    const kit = kitBankData(7), plain = kitBankData(8);
    const withKits = () => simWith(base, Array.from({ length: 14 }, (_, b) => (b === 8 ? [...kit] : b === 9 ? [...plain] : b === 10 ? [0xFF, 0xFF] : [b + 1])));
    sim = withKits();
    const l = await ln.listSongs(sim.link);
    ok(l.list.length === 1 && l.list[0].name === "DEVSONG" && l.free === fs.freeBlocks(base) && !sim.held && sim.resets === 0, "the song list; the Game Boy runs again, not restarted");

    /* Review Focus 2: not started is said plainly */
    sim.started = false;
    ok(await rejects(ln.listSongs(sim.link), /not started/), "not started is said plainly");
    sim.started = true;

    const dl = await ln.downloadSong(sim.link, 2);
    const kitsOnDevice = fs.songKitBanks(base, 2).map(b => Uint8Array.from({ length: 0x4000 }, (_, i) => sim.bankByte(b, i)));
    ok(dl.name === "DEVSONG-v07.lsdprj" && eq(dl.bytes, fs.buildSongFile(base, 2, kitsOnDevice)) && !sim.held && sim.resets === 0, "a song downloads with its kit banks");
    ok(await rejects(ln.downloadSong(sim.link, 9), /empty/) && !sim.held, "an empty slot: said, and the Game Boy runs again");

    /* upload: a song whose two kits are in banks 8 and 9 of this device */
    const src = emptySav(); addPacked(src, 0, testSong(1), "UP", 3);
    const upFile = fs.buildSongFile(src, 0, [kit, plain]);
    const crcs = await ln.bankCrcs(sim.link, 14);
    ok(crcs.length === 14 && crcs[8] === fs.crc32(kit), "the bank CRCs of the device");
    const slot = await ln.uploadSong(sim.link, upFile, crcs);
    const expect = fs.importSong(base, fs.parseSongFile(upFile), [8, 9]).sav;
    ok(slot === 0 && eq(sim.sram.subarray(0x8000), expect.subarray(0x8000)) && eq(sim.sram.subarray(0, 0x8000), base.subarray(0, 0x8000)), "an upload: the device's file system is the imported one, the edited song untouched");
    ok(sim.resets === 1 && !sim.held, "... and LSDj starts again");
    ok(sim.log.at(-1) >= fs.ACTIVE && sim.log.at(-1) < fs.BLOCKS && sim.log.findIndex(a => a >= fs.ACTIVE && a < fs.BLOCKS) > sim.log.findLastIndex(a => a >= fs.BLOCKS), "the block table is written after every block");

    /* Review Focus 1: a failing write leaves the block table alone */
    sim = withKits(); sim.failFrom = 5;
    ok(await rejects(ln.uploadSong(sim.link, upFile, crcs), /crc/) && eq(sim.sram.subarray(fs.ACTIVE, fs.BLOCKS), base.subarray(fs.ACTIVE, fs.BLOCKS)) && !sim.held && sim.resets === 0, "a failing write leaves the block table alone; no restart");

    /* Review Focus 3: a damaged file sends nothing */
    sim = withKits();
    ok(await rejects(Promise.resolve().then(() => ln.uploadSong(sim.link, base.subarray(0, 0x20000), crcs)), /song/) && sim.writes === 0 && !sim.held, "a damaged file sends nothing (a .sav dropped as a song)");
    ok(await rejects(Promise.resolve().then(() => ln.uploadSong(sim.link, new Uint8Array(100), crcs)), /too short/), "... nor a short file");
    const alien = fs.buildSongFile(src, 0, [kitBankData(99), plain]);
    ok(await rejects(Promise.resolve().then(() => ln.uploadSong(sim.link, alien, crcs)), /1 kit/) && sim.writes === 0, "a kit the FM-1 does not have: refused with the count, nothing sent");

    /* Review Focus 4: an upload reads the header again */
    sim = withKits();
    await ln.listSongs(sim.link);
    sim.onHold = () => { sim.onHold = null; addPacked(sim.sram, 0, testSong(6), "LATER", 1); };   // saved in LSDj meanwhile
    ok((await ln.uploadSong(sim.link, upFile, crcs)) === 1 && fs.songs(sim.sram).map(s => s.name).join() === "LATER,UP,DEVSONG" && eq(fs.unpackSong(sim.sram, 0), testSong(6)), "an upload reads the header again: a song saved meanwhile is kept");

    sim = withKits();
    await ln.deleteSong(sim.link, 2);
    ok(fs.songs(sim.sram).length === 0 && sim.resets === 1, "a song is deleted");

    sim = withKits();
    const whole = await ln.downloadSav(sim.link);
    ok(eq(whole, base) && !sim.held && sim.resets === 0, "the whole save downloads");
    const other = emptySav(); addPacked(other, 1, testSong(2), "NEW", 1); other[5] = 0x99;
    await ln.uploadSav(sim.link, other);
    ok(eq(sim.sram, other) && sim.resets === 1, "a save is restored; only changed pieces are written");
    ok(sim.log.length > 0 && sim.log.length < 1500, "... the changed pieces, not all 6554");
    ok(await rejects(Promise.resolve().then(() => ln.uploadSav(sim.link, other.subarray(0, 0x8000))), /131072/), "a save of the wrong size is refused");
    const blank = new Uint8Array(fs.SAV_SIZE);
    ok(await rejects(Promise.resolve().then(() => ln.uploadSav(sim.link, blank)), /file system mark/), "a file that is not an LSDj save is refused");
})();

/* ---- the page, inlined as web/make_site.py does: one script that compiles ---- */
{
    const HERE = new URL(".", import.meta.url).pathname;
    if (!existsSync(HERE + "lsdj.html")) console.log("== skip: no web/lsdj.html yet");
    else {
        const strip = s => s.replace(/^export\s+/gm, "").replace(/^import .*?;\n/gm, "");
        const html = readFileSync(HERE + "lsdj.html", "utf8");
        const lib = strip(readFileSync(HERE + "lsdjfs.js", "utf8")) + "\n" + strip(readFileSync(HERE + "lsdjlink.js", "utf8"));
        const m = /<script>([\s\S]*)<\/script>/.exec(html.replace("/*LIB*/", lib));
        let compiled = false;
        try { new vm.Script(m[1]); compiled = true; } catch (e) { console.log("   " + e.message); }
        ok(html.split("/*LIB*/").length === 2 && compiled && !/^(import|export) /m.test(m[1]), "lsdj.html with the modules inlined is one script that compiles");
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `node web/test_lsdj.mjs`
Expected: FAIL, `Cannot find module '.../web/lsdjlink.js'`.

- [ ] **Step 3: Write the implementation**

Create `web/lsdjlink.js`:

```js
// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// The LSDj build's transfer commands (the console's `ble WORDS`, firmware/src/lsdj/lsdj_xfer.c) from a page, and
// what the song page does with them. No browser API here: the page gives Link a function that sends bytes and feeds it what arrives.
//   a command: "ble WORDS\r" -> the console echoes it, then "\r\n", one line "ok ..." or "err ...", "\r\n> "
//   (the console line is 64 characters: a write carries 20 bytes)
//   the save RAM is read and written only while the Game Boy is held; every flow ends with `go`
import { ACTIVE, BANK, BLOCK, BLOCKS, FAT, NBLOCKS, SAV_SIZE, SLOTS, buildSongFile, crc32, freeBlocks, importSong, parseSongFile, removeSong, slotBlocks, songFileName, songKitBanks, songs } from "./lsdjfs.js";

const RD = 256, WR = 20, TRIES = 3, FS_HEAD = 0x8000;
const hx = (v, digits = 1) => v.toString(16).toUpperCase().padStart(digits, "0");
const ascii = s => Uint8Array.from(s, c => c.charCodeAt(0));

export function hexBytes(bytes) {
    let s = "";
    for (const b of bytes) s += hx(b, 2);
    return s;
}

export function unhex(s) {
    if (s.length % 2 || /[^0-9A-Fa-f]/.test(s)) return null;
    const out = new Uint8Array(s.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(2 * i, 2), 16);
    return out;
}

export function crc16(bytes) {                           // XMODEM: 0x1021, start 0
    let c = 0;
    for (const b of bytes) {
        c ^= b << 8;
        for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    }
    return c;
}

export class Link {
    constructor(send, timeoutMs = 2000) {
        Object.assign(this, { send, timeoutMs, text: "", wake: null, queue: Promise.resolve() });
    }

    feed(bytes) {                                        // what arrived from the FM-1
        for (const b of bytes) this.text += String.fromCharCode(b);
        if (this.wake) this.wake();
    }

    until(re) {                                          // the received text matches re, or the time is up
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => { this.wake = null; reject(new Error("the FM-1 does not answer")); }, this.timeoutMs);
            this.wake = () => {
                const m = re.exec(this.text);
                if (m) { clearTimeout(timer); this.wake = null; resolve(m); }
            };
            this.wake();
        });
    }

    async sync() {                                       // an empty line: the console answers with its prompt
        this.text = "";
        await this.send(ascii("\r"));
        await this.until(/> $/);
    }

    /* one command -> what follows "ok" in its reply; "err ..." and silence throw. Commands run one at a time. */
    cmd(line) {
        const run = async () => {
            this.text = "";
            await this.send(ascii(`ble ${line}\r`));
            await this.until(/\r\n> $/);
            const m = /(?:^|\n)(ok|err)(?: ([^\r\n]*))?\r\n> $/.exec(this.text);
            if (!m) throw new Error("this FM-1's firmware has no transfer command: install the LSDj build that has it");
            if (m[1] === "err") throw new Error("FM-1: " + (m[2] || "error"));
            return m[2] || "";
        };
        const p = this.queue.then(run, run);
        this.queue = p.catch(() => {});
        return p;
    }

    async info() {
        const m = /^v([0-9A-F]+) started ([01]) held ([01]) banks ([0-9A-F]+)$/.exec(await this.cmd("info"));
        if (!m) throw new Error("FM-1: an info reply this page does not know");
        return { version: parseInt(m[1], 16), started: m[2] === "1", held: m[3] === "1", banks: parseInt(m[4], 16) };
    }

    hold() { return this.cmd("hold"); }
    go(reset = false) { return this.cmd(reset ? "go reset" : "go"); }

    async data(line, n) {                                // a reply "HEX CRC": checked, asked again when damaged
        let last;
        for (let t = 0; t < TRIES; t++) {
            try {
                const [h, c] = (await this.cmd(line)).split(" ");
                const d = unhex(h || "");
                if (d && d.length === n && parseInt(c, 16) === crc16(d)) return d;
                last = new Error("the FM-1 sent damaged data");
            } catch (e) {
                if (!/does not answer/.test(e.message)) throw e;
                last = e;
                await this.sync().catch(() => {});
            }
        }
        throw last;
    }

    async read(addr, len, progress = () => {}) {         // the save RAM (held)
        const out = new Uint8Array(len);
        for (let o = 0; o < len; o += RD) {
            const n = Math.min(RD, len - o);
            out.set(await this.data(`r ${hx(addr + o)} ${hx(n)}`, n), o);
            progress(o + n, len);
        }
        return out;
    }

    async write(addr, bytes, progress = () => {}) {      // the save RAM (held); a piece is sent again on a bad CRC
        for (let o = 0; o < bytes.length; o += WR) {
            const d = bytes.subarray(o, o + WR), line = `w ${hx(addr + o)} ${hexBytes(d)} ${hx(crc16(d), 4)}`;
            for (let t = 0; ; t++) {
                try { await this.cmd(line); break; } catch (e) {
                    if (t + 1 === TRIES || !/FM-1: (crc|hex)|does not answer/.test(e.message)) throw e;
                    if (/does not answer/.test(e.message)) await this.sync().catch(() => {});
                }
            }
            progress(Math.min(o + WR, bytes.length), bytes.length);
        }
    }

    async readBank(bank, progress = () => {}) {          // a whole 16 KiB ROM bank
        const out = new Uint8Array(BANK);
        for (let o = 0; o < BANK; o += RD) {
            out.set(await this.data(`b ${hx(bank)} ${hx(o)} ${hx(RD)}`, RD), o);
            progress(o + RD, BANK);
        }
        return out;
    }

    async bankCrc(bank) { return parseInt(await this.cmd("bc " + hx(bank)), 16); }
}

/* the Game Boy held while fn runs; it always runs again after, restarted if fn changed the save and succeeded */
async function whileHeld(link, restart, fn) {
    await link.hold();
    let done = false;
    try {
        const r = await fn();
        done = true;
        return r;
    } finally {
        await link.go(done && restart).catch(() => {});
    }
}

async function fsHeader(link) {                          // a save image with only the file system's first 512 B read
    const sav = new Uint8Array(SAV_SIZE);
    sav.set(await link.read(FS_HEAD, BLOCK), FS_HEAD);
    return sav;
}

const samePiece = (a, b, o, n) => { for (let i = o; i < o + n; i++) if (a[i] !== b[i]) return false; return true; };

/* old -> sav on the device: the blocks that got an owner whole, then the changed pieces of the names and versions,
   the active slot and the block table last (until then LSDj's file system is the old one) */
async function writeChange(link, old, sav, progress = () => {}) {
    const pieces = [];
    for (let b = 0; b < NBLOCKS; b++)
        if (sav[FAT + b] !== old[FAT + b] && sav[FAT + b] < SLOTS) pieces.push([BLOCKS + BLOCK * b, BLOCK]);
    for (const [a, e] of [[FS_HEAD, ACTIVE], [ACTIVE, BLOCKS]])
        for (let o = a; o < e; o += WR) {
            const n = Math.min(WR, e - o);
            if (!samePiece(old, sav, o, n)) pieces.push([o, n]);
        }
    let i = 0;
    for (const [o, n] of pieces) {
        await link.write(o, sav.subarray(o, o + n));
        progress(++i, pieces.length);
    }
}

export function listSongs(link) {
    return whileHeld(link, false, async () => {
        const sav = await fsHeader(link);
        return { sav, list: songs(sav), free: freeBlocks(sav) };
    });
}

export function downloadSong(link, slot, progress = () => {}) {
    return whileHeld(link, false, async () => {
        const sav = await fsHeader(link), blocks = slotBlocks(sav, slot);
        if (!blocks.length) throw new Error("the slot is empty");
        for (const b of blocks) sav.set(await link.read(BLOCKS + BLOCK * b, BLOCK), BLOCKS + BLOCK * b);
        const banks = songKitBanks(sav, slot), kits = [];
        for (const [i, bank] of banks.entries()) kits.push(await link.readBank(bank, (a, n) => progress(i * n + a, banks.length * n)));
        return { name: songFileName(sav, slot), bytes: buildSongFile(sav, slot, kits) };
    });
}

/* bankCrcs: the CRC-32 of each ROM bank of this FM-1 (bankCrcs() below), to find the song's kits there */
export function uploadSong(link, bytes, romCrcs, progress = () => {}) {
    const file = parseSongFile(bytes);                   // a damaged file stops here: nothing is sent
    const kitBanks = file.kits.map(k => romCrcs.indexOf(crc32(k)));
    const missing = kitBanks.filter(b => b < 0).length;
    if (missing)
        throw new Error(`this song brings ${missing} kit${missing > 1 ? "s" : ""} that this FM-1 does not have (custom kits arrive with saving to flash)`);
    return whileHeld(link, true, async () => {
        const old = await fsHeader(link), { sav, slot } = importSong(old, file, kitBanks);
        await writeChange(link, old, sav, progress);
        return slot;
    });
}

export function deleteSong(link, slot) {
    return whileHeld(link, true, async () => {
        const old = await fsHeader(link);
        await writeChange(link, old, removeSong(old, slot));
    });
}

export function downloadSav(link, progress = () => {}) {
    return whileHeld(link, false, () => link.read(0, SAV_SIZE, progress));
}

export function uploadSav(link, bytes, progress = () => {}) {
    if (bytes.length !== SAV_SIZE) throw new Error("not an LSDj save: it must be 131072 bytes");
    if (bytes[0x813E] !== 0x6A || bytes[0x813F] !== 0x6B) throw new Error("not an LSDj save: no file system mark");
    return whileHeld(link, true, async () => {
        const old = await link.read(0, SAV_SIZE, (a, n) => progress(a, 2 * n));
        const pieces = [];
        for (let o = 0; o < SAV_SIZE; o += WR) if (!samePiece(old, bytes, o, Math.min(WR, SAV_SIZE - o))) pieces.push(o);
        let i = 0;
        for (const o of pieces) {
            await link.write(o, bytes.subarray(o, Math.min(o + WR, SAV_SIZE)));
            progress(SAV_SIZE + Math.round(SAV_SIZE * ++i / pieces.length), 2 * SAV_SIZE);
        }
    });
}

export async function bankCrcs(link, n) {
    const out = [];
    for (let b = 0; b < n; b++) out.push(await link.bankCrc(b));
    return out;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `build/tests/lsdj_xfer_test >/dev/null && node web/test_lsdj.mjs | tail -40`
Expected: every line ends `ok` (among them `the simulated FM-1 answers NN commands as the firmware does`), one `== skip: no web/lsdj.html yet`, the last line `test_lsdj: all passed`.

- [ ] **Step 5: Commit**

```bash
git status --short
git add web/lsdjlink.js web/test_lsdj.mjs
git commit -m "lsdj: the console link and the song page's flows, against a simulated FM-1 that answers as the firmware"
```

---

### Task 5: The page (`web/lsdj.html`) and the site

**Files:**
- Create: `web/lsdj.html`
- Modify: `web/make_site.py` (the docstring's list; the page written next to the installer)

**Interfaces:**
- Consumes: Task 4's `Link`, `listSongs`, `downloadSong`, `uploadSong`, `deleteSong`, `downloadSav`, `uploadSav`, `bankCrcs` (inlined at the `/*LIB*/` mark; the page's own code is inside one function so its names cannot collide).
- Produces: `OUT_DIR/webapp/lsdj/index.html`, self-contained.

- [ ] **Step 1: Write the page**

Create `web/lsdj.html`:

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LSDj songs</title>
<style>
  :root { --bg: #fff; --fg: #000; --dim: #777; --line: #ddd; --bad: #b00020; }
  @media (prefers-color-scheme: dark) {
    :root:not([data-theme="light"]) { --bg: #000; --fg: #fff; --dim: #888; --line: #333; --bad: #ff6b81; }
  }
  :root[data-theme="dark"] { --bg: #000; --fg: #fff; --dim: #888; --line: #333; --bad: #ff6b81; }
  body { margin: 0; background: var(--bg); color: var(--fg);
         font: 16px/1.6 -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial, sans-serif; }
  main { max-width: 560px; margin: 0 auto; padding: 48px 16px; }
  h1 { font-size: 24px; font-weight: 600; margin: 0 0 24px; }
  p { margin: 0 0 16px; }
  button { font: inherit; background: var(--fg); color: var(--bg); border: 0; padding: 8px 20px; cursor: pointer; }
  button.quiet { background: none; color: var(--fg); padding: 2px 8px; text-decoration: underline; }
  button:disabled { opacity: .3; cursor: default; }
  table { width: 100%; border-collapse: collapse; margin: 0 0 16px; font-variant-numeric: tabular-nums; }
  th, td { text-align: left; padding: 6px 8px 6px 0; border-bottom: 1px solid var(--line); }
  th { font-weight: 400; color: var(--dim); font-size: 13px; }
  td.name { font-family: ui-monospace, Menlo, monospace; white-space: pre; }
  td.act { text-align: right; white-space: nowrap; padding-right: 0; }
  #drop { border: 1px dashed var(--dim); color: var(--dim); padding: 24px 16px; text-align: center; }
  #drop.over { border-color: var(--fg); color: var(--fg); }
  #status { margin-top: 16px; min-height: 1.6em; }
  #status.bad { color: var(--bad); }
  .small { color: var(--dim); font-size: 13px; }
  progress { width: 100%; height: 4px; margin-top: 16px; }
</style>
</head>
<body>
<main>
  <h1>LSDj on the FM-1: songs</h1>
  <p class="small">Chrome or Edge on a computer, the FM-1 on USB, LSDj started (PLAY). The song you are editing
    is not in the list until you save it in LSDj: PROJECT, LOAD/SAVE SONG, SAVE. After an upload LSDj starts
    again; load the song there: PROJECT, LOAD/SAVE SONG, LOAD.</p>
  <p class="small">Nothing is kept when the FM-1 is switched off yet: download what you want to keep.</p>
  <p><button id="connect">Connect</button> <button id="refresh" class="quiet" hidden>Refresh</button></p>
  <div id="panel" hidden>
    <table>
      <thead><tr><th>Slot</th><th>Name</th><th>Version</th><th>Blocks</th><th></th></tr></thead>
      <tbody id="list"></tbody>
    </table>
    <p class="small" id="room"></p>
    <p id="drop">Drop .lsdprj or .lsdsng files here, or <button id="pick" class="quiet">choose files</button></p>
    <input type="file" id="files" accept=".lsdprj,.lsdsng" multiple hidden>
    <p><button id="savget">Download the whole save (.sav)</button>
       <button id="savput" class="quiet">Restore a save…</button></p>
    <input type="file" id="savfile" accept=".sav" hidden>
  </div>
  <div id="status" role="status"></div>
  <progress id="bar" value="0" max="1" hidden></progress>
</main>
<script>
/*LIB*/
(function () {
    const $ = id => document.getElementById(id);
    let uiLink = null, uiCrcs = [], uiBusy = false;

    const say = (text, bad) => { $("status").textContent = text; $("status").className = bad ? "bad" : ""; };
    const bar = (done, total) => { $("bar").hidden = false; $("bar").max = total; $("bar").value = done; };
    const plain = e => (/not started/.test(e.message) ? "LSDj is not running: press PLAY on the FM-1, then Refresh" : e.message);
    function buttons() {
        for (const b of document.querySelectorAll("#panel button, #refresh")) b.disabled = uiBusy || !uiLink;
        $("connect").disabled = uiBusy || !!uiLink;
    }
    function saveFile(name, bytes) {
        const a = document.createElement("a");
        a.href = URL.createObjectURL(new Blob([bytes]));
        a.download = name;
        a.click();
        setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    }

    /* one thing at a time; the buttons are off while it runs; what went wrong is said in a sentence */
    async function job(what, fn) {
        if (uiBusy || !uiLink) return;
        uiBusy = true;
        buttons();
        say(what + "…");
        try {
            say((await fn()) || what + ": done");
        } catch (e) {
            say(what + ": " + plain(e), true);
        } finally {
            uiBusy = false;
            $("bar").hidden = true;
            buttons();
        }
    }

    async function draw() {
        const { list, free } = await listSongs(uiLink);
        $("list").textContent = "";
        for (const s of list) {
            const tr = document.createElement("tr");
            const cell = (text, cls) => { const td = document.createElement("td"); td.textContent = text; if (cls) td.className = cls; tr.append(td); return td; };
            cell(s.slot.toString(16).toUpperCase().padStart(2, "0"));
            cell(s.name || "(no name)", "name");
            cell(s.version.toString(16).toUpperCase().padStart(2, "0"));
            cell(String(s.blocks));
            const act = cell("", "act");
            const get = document.createElement("button"), del = document.createElement("button");
            get.textContent = "Download";
            get.className = del.className = "quiet";
            get.onclick = () => job("Download " + (s.name || "song"), async () => {
                const f = await downloadSong(uiLink, s.slot, bar);
                saveFile(f.name, f.bytes);
                return "Downloaded " + f.name;
            });
            del.textContent = "Delete";
            del.onclick = () => {
                if (!confirm(`Delete ${s.name || "this song"} from the FM-1? LSDj starts again.`)) return;
                job("Delete " + (s.name || "song"), async () => { await deleteSong(uiLink, s.slot); await draw(); });
            };
            act.append(get, del);
            $("list").append(tr);
        }
        if (!list.length) {
            const tr = document.createElement("tr"), td = document.createElement("td");
            td.colSpan = 5;
            td.className = "small";
            td.textContent = "No saved songs.";
            tr.append(td);
            $("list").append(tr);
        }
        $("room").textContent = `${list.length} of 32 songs, ${free} of 191 blocks free.`;
        buttons();
    }

    async function upload(files) {
        if (!files.length) return;
        await job("Upload", async () => {
            const done = [];
            try {
                for (const f of files) {
                    say(`Upload ${f.name}…`);
                    try {
                        await uploadSong(uiLink, new Uint8Array(await f.arrayBuffer()), uiCrcs, bar);
                    } catch (e) {
                        throw new Error(`${f.name}: ${plain(e)}` + (done.length ? ` (uploaded before it: ${done.join(", ")})` : ""));
                    }
                    done.push(f.name);
                }
            } finally {
                await draw().catch(() => {});
            }
            return `Uploaded ${done.join(", ")}. Load it in LSDj: PROJECT, LOAD/SAVE SONG, LOAD.`;
        });
    }

    $("connect").onclick = async () => {
        if (!navigator.serial) return say("This browser has no Web Serial: use Chrome or Edge on a computer.", true);
        let port;
        try {
            port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x1209, usbProductId: 0x0001 }] });
        } catch (e) {
            return say("No FM-1 chosen.", true);
        }
        try {
            await port.open({ baudRate: 115200 });
            await port.setSignals({ dataTerminalReady: true });   // the console answers only with DTR on
            const writer = port.writable.getWriter(), link = new Link(b => writer.write(b));
            (async () => {
                const reader = port.readable.getReader();
                for (;;) {
                    const { value, done } = await reader.read();
                    if (done) break;
                    link.feed(value);
                }
            })().catch(() => {}).finally(() => {
                uiLink = null;
                $("panel").hidden = $("refresh").hidden = true;
                buttons();
                say("The FM-1 was disconnected.", true);
            });
            say("Connecting…");
            await link.sync();
            const info = await link.info();
            uiCrcs = await bankCrcs(link, info.banks);
            uiLink = link;
            $("panel").hidden = $("refresh").hidden = false;
            buttons();
            await job("Read the song list", draw);
        } catch (e) {
            say(plain(e), true);
            await port.close().catch(() => {});
        }
    };
    $("refresh").onclick = () => job("Read the song list", draw);
    $("pick").onclick = () => $("files").click();
    $("files").onchange = () => { upload([...$("files").files]); $("files").value = ""; };
    $("drop").ondragover = e => { e.preventDefault(); $("drop").classList.add("over"); };
    $("drop").ondragleave = () => $("drop").classList.remove("over");
    $("drop").ondrop = e => { e.preventDefault(); $("drop").classList.remove("over"); upload([...e.dataTransfer.files]); };
    $("savget").onclick = () => job("Download the save", async () => {
        const d = new Date(), stamp = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, "0")}${String(d.getDate()).padStart(2, "0")}`;
        saveFile(`lsdj-fm1-${stamp}.sav`, await downloadSav(uiLink, bar));
        return "Downloaded the save.";
    });
    $("savput").onclick = () => $("savfile").click();
    $("savfile").onchange = () => {
        const f = $("savfile").files[0];
        $("savfile").value = "";
        if (!f || !confirm(`Replace everything in LSDj on the FM-1 with ${f.name}? The song being edited and all saved songs are replaced.`)) return;
        job("Restore the save", async () => { await uploadSav(uiLink, new Uint8Array(await f.arrayBuffer()), bar); await draw(); return "Restored " + f.name + ". LSDj started again."; });
    };
    buttons();
})();
</script>
</body>
</html>
```

- [ ] **Step 2: Check that the page compiles with the modules inlined**

Run: `node web/test_lsdj.mjs | tail -3`
Expected: `lsdj.html with the modules inlined is one script that compiles ... ok`, then `test_lsdj: all passed` (the skip line is gone).

- [ ] **Step 3: Add the page to the site**

In `web/make_site.py`, in the docstring, after the line `  webapp/editor/index.html    editor.html (+ fukiai.ttf, FUKIAI-LICENSE.txt)` add:

```
  webapp/lsdj/index.html      lsdj.html, self-contained (lsdjfs.js, lsdjlink.js inlined): the LSDj build's songs
```

and in `main`, just before the line `    (out / "index.html").write_text(`, add:

```python
    page = (HERE / "lsdj.html").read_text(encoding="utf-8")      # the LSDj build's song page (the USB console)
    if page.count("/*LIB*/") != 1:
        raise SystemExit("lsdj.html must contain /*LIB*/ once; update make_site.py")
    songs_lib = strip_module((HERE / "lsdjfs.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((HERE / "lsdjlink.js").read_text(encoding="utf-8"))
    (out / "webapp" / "lsdj").mkdir(parents=True, exist_ok=True)
    (out / "webapp" / "lsdj" / "index.html").write_text(page.replace("/*LIB*/", songs_lib), encoding="utf-8")
```

- [ ] **Step 4: Build the site and look at the page**

Run:

```bash
python3 web/make_site.py build/lsdj-private.fwsc lsdj-9 build/sites/LSDJ9
grep -c "class Link" build/sites/LSDJ9/webapp/lsdj/index.html
(python3 -m http.server 8004 -d build/sites/LSDJ9 >/dev/null 2>&1 &)
```

Expected: `site: build/sites/LSDJ9: ...`, then `1`. Open `http://localhost:8004/webapp/lsdj/` in Chrome: the heading, the two notes and a Connect button show; the browser console has no errors; in both light and dark appearance the text is readable. (No FM-1 is needed for this check.)

- [ ] **Step 5: Commit**

```bash
git status --short
git add web/lsdj.html web/make_site.py
git commit -m "lsdj: the song page (Web Serial): list, download, upload, delete, the whole save; on the site at webapp/lsdj"
```

---

### Task 6: The real LSDj loads what the page writes (private test)

**Files:**
- Create: `tests/lsdj/lsdj_song_test.mjs`
- Modify: `tests/run_tests.sh` (one line in the `node` block)

**Interfaces:**
- Consumes: Task 3's `lsdjfs.js`; the host harness `build/host/lsdj_host` (built by `tools/lsdj_rom.py` during a `FELUCCA_LSDJ=1` build or Task 2 step 2), `build/lsdj/boot.sav` (LSDj started, empty song) and `build/lsdj/demo.sav` (the demo song being edited, nothing saved); `LSDJ_ROM`.
- Produces: nothing other tasks use. Everything it writes goes to `build/tests/` (git-ignored).

Facts measured on LSDj 9.4.2 (2026-10-11), which the key lists below rest on: from the song screen, SELECT + UP opens PROJECT; ten times DOWN reaches LOAD/SAVE SONG; A opens the file screen with the cursor on LOAD (RIGHT: SAVE); A enters the slot list; on SAVE, A opens SAVE AS with the cursor on OK, A saves (about 10 s of Game Boy time); on LOAD, A loads the slot and returns to the song screen. After a load from a slot, the working memory is the same in every run except its timer bytes in 0x3FB0..0x3FCF.

- [ ] **Step 1: Write the test**

Create `tests/lsdj/lsdj_song_test.mjs`:

```js
// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
//
// With the owner's ROM only (never stored in this repo): the real LSDj, on the firmware's own emulator
// (tests/lsdj/lsdj_host.c), loads a song that web/lsdjfs.js put into a save. Run from the repo root:
//   LSDJ_ROM=your.gb node tests/lsdj/lsdj_song_test.mjs
// 1. LSDj saves the demo song to slot 0 itself (keys on its file screen)
// 2. lsdjfs exports that slot as a song file and adds it twice to the empty start-up save (slots 0 and 1, so
//    slot 1 lies in other blocks and its jumps are rewritten)
// 3. LSDj loads slot 1 of that save; its working memory must equal what LSDj's own load of its own slot gives,
//    outside the timer bytes 0x3FB0..0x3FCF; and the song plays
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { BLOCKS, buildSongFile, importSong, parseSongFile, slotBlocks, songKitBanks, songs, unpackSong } from "../../web/lsdjfs.js";

const ROM = process.env.LSDJ_ROM, HOST = "build/host/lsdj_host", BOOT = "build/lsdj/boot.sav", DEMO = "build/lsdj/demo.sav";
if (!ROM || ![HOST, BOOT, DEMO].every(existsSync)) {
    console.log("lsdj_song_test: skipped (LSDJ_ROM, build/host/lsdj_host and build/lsdj/*.sav: build with FELUCCA_LSDJ=1 first)");
    process.exit(0);
}
let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(72)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };
mkdirSync("build/tests", { recursive: true });
const T = "build/tests/song_";

/* keys for the harness: a letter is pressed for 4 frames, 10 frames apart; "s+u": s held, u pressed inside it;
   a number waits that many frames. Letters: l r u d a b s(elect) t(start). */
function run(save, out, steps, tail = 60) {
    const args = [];
    let f = 60;
    for (const k of steps) {
        if (typeof k === "number") f += k;
        else if (k.includes("+")) {
            const [m, x] = k.split("+");
            args.push("-k", `${f}:${f + 8}:${m}`, "-k", `${f + 2}:${f + 6}:${x}`);
            f += 14;
        } else {
            args.push("-k", `${f}:${f + 4}:${k}`);
            f += 10;
        }
    }
    const text = execFileSync(HOST, [ROM, String(f + tail), "-s", save, "-o", out, ...args], { encoding: "utf8" });
    return { text, sav: new Uint8Array(readFileSync(out)) };
}
const FILE_SCREEN = ["s+u", ...Array(10).fill("d"), "a", 60];        // PROJECT, down to LOAD/SAVE SONG, A

/* 1. LSDj's own save of the demo song: RIGHT (SAVE), A (the slot list), A (SAVE AS), A (OK) */
const ref = run(DEMO, T + "ref.sav", [...FILE_SCREEN, "r", "a", 60, "a", 60, "a", 900]).sav;
const list = songs(ref);
ok(list.length === 1 && list[0].slot === 0 && list[0].blocks >= 1 && ref[0x8140] === 0, "LSDj saved the demo song to slot 0");
const song = unpackSong(ref, 0);
ok(song !== null, "lsdjfs unpacks LSDj's own slot to a whole song");

/* 2. out as a file, into the empty save twice */
ok(songKitBanks(ref, 0).length === 0, "the demo song uses no kits");
const file = parseSongFile(buildSongFile(ref, 0, []));
const boot = new Uint8Array(readFileSync(BOOT));
ok(songs(boot).length === 0, "the start-up save has no songs");
const two = importSong(importSong(boot, file).sav, file);
ok(two.slot === 1 && slotBlocks(two.sav, 1)[0] === list[0].blocks, "the second copy lies in the blocks after the first");
ok(unpackSong(two.sav, 1).every((v, i) => v === song[i]), "it unpacks to the same song there");
writeFileSync(T + "put.sav", two.sav);

/* 3. LSDj loads it: A (the slot list, LOAD), DOWN to slot 1, A */
const own = run(T + "ref.sav", T + "own.sav", [...FILE_SCREEN, "a", 60, "a", 600]).sav;
const got = run(T + "put.sav", T + "got.sav", [...FILE_SCREEN, "a", 60, "d", "a", 600]).sav;
ok(got[0x8140] === 1, "LSDj made slot 1 its active song");
const diff = [];
for (let i = 0; i < 0x8000; i++) if (got[i] !== own[i] && (i < 0x3FB0 || i > 0x3FCF)) diff.push(i);
if (diff.length) console.log("   differing addresses: " + diff.slice(0, 16).map(a => a.toString(16)).join(" "));
ok(diff.length === 0, "LSDj's working memory equals its own load of its own save (outside the timers)");
let before = 0;
for (let i = 0; i < 0x8000; i++) if (boot[i] !== got[i]) before++;
ok(before > 100, "... and it is not the empty song it started from");
ok(got.subarray(BLOCKS).every((v, i) => v === two.sav[BLOCKS + i]), "LSDj left the blocks as they were written");
const play = execFileSync(HOST, [ROM, "600", "-s", T + "got.sav", "-k", "60:64:t"], { encoding: "utf8" });
const peak = /audio peak (\d+)/.exec(play);
ok(peak && +peak[1] >= 2000, "START plays the loaded song");

console.log(failed ? `lsdj_song_test: ${failed} FAILED` : "lsdj_song_test: all passed");
process.exit(failed ? 1 : 0);
```

- [ ] **Step 2: Run it**

Run: `LSDJ_ROM=$HOME/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb node tests/lsdj/lsdj_song_test.mjs`
Expected: eleven lines ending `ok`, then `lsdj_song_test: all passed`. (`build/host/lsdj_host`, `boot.sav` and `demo.sav` exist after Task 2's build.)

If "LSDj's working memory equals ..." fails, the printed addresses say where: a difference inside 0x3000..0x3FAF or 0x4000..0x7FFF is song data, so `lsdjfs.importSong` is wrong and must be fixed; do not widen the ignored range.

- [ ] **Step 3: Verify it skips without the ROM**

Run: `env -u LSDJ_ROM node tests/lsdj/lsdj_song_test.mjs; echo "exit $?"`
Expected: `lsdj_song_test: skipped (...)`, `exit 0`.

- [ ] **Step 4: Add the test to the suite**

In `tests/run_tests.sh`, inside the `if command -v node` block, after the `test_lsdj.mjs` line, add:

```sh
    run "LSDj songs with the real ROM: LSDj loads what the page writes (skipped without LSDJ_ROM)" node tests/lsdj/lsdj_song_test.mjs
```

- [ ] **Step 5: Commit**

```bash
git status --short
git add tests/lsdj/lsdj_song_test.mjs tests/run_tests.sh
git commit -m "lsdj: with the owner's ROM, the real LSDj loads and plays a song the page's code put into the save"
```

---

### Task 7: The build for the device and its check sheet

**Files:**
- Create: `docs/lsdj/DEVICE_STEPS_TRANSFER.md`

**Interfaces:**
- Consumes: everything above.
- Produces: `build/lsdj-private.fwsc`, the site in `build/sites/LSDJ9` served on `http://localhost:8004/`, and the check sheet for the owner.

- [ ] **Step 1: Full suite and a fresh build**

Run:

```bash
export LSDJ_ROM=$HOME/Downloads/lsdj9_4_2-stable/lsdj9_4_2.gb
PYTHON=$HOME/Desktop/dev/fm1-lsdj/build/venv/bin/python FELUCCA_LSDJ=1 ./build.sh 2>&1 | tail -8
sh tests/run_tests.sh 2>&1 | grep -E "FAIL|FAILED|skip|passed" | tail -30
python3 tools/check_update_path.py --sources
git diff --stat lsdj-emu -- tests/ble/update_path.ref
```

Expected: the build's `ok    update path: unchanged` and `ok    no Game Boy reachable at boot (only through ble_key)`; no `FAIL` in the suite; `update path sources: identical to v1.0.5.2`; the last command prints nothing (the reference was not touched).

- [ ] **Step 2: Rebuild the site with this firmware**

Run:

```bash
python3 web/make_site.py build/lsdj-private.fwsc lsdj-9 build/sites/LSDJ9
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8004/webapp/lsdj/ || (python3 -m http.server 8004 -d build/sites/LSDJ9 >/dev/null 2>&1 &)
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8004/webapp/installer/
```

Expected: `200` for both pages (the first `curl` starts the server if Task 5's is gone).

- [ ] **Step 3: Write the check sheet**

Create `docs/lsdj/DEVICE_STEPS_TRANSFER.md`:

```markdown
# LSDj on the FM-1: song transfer: device check

Firmware: `build/lsdj-private.fwsc` (private: it holds your LSDj ROM). Pages, served from this Mac:
installer `http://localhost:8004/webapp/installer/`, songs `http://localhost:8004/webapp/lsdj/`. Use Chrome.

Nothing is saved on the FM-1 yet (that is the next step): what you upload lasts until power-off.

1. **Install.** Open the installer page and install. The FM-1 restarts into the LSDj start screen.
2. **Start LSDj.** Press PLAY. Wait for the song screen.
3. **Connect.** Open the songs page, press Connect, choose the FM-1 in Chrome's list. Expected: "No saved songs."
   and "0 of 32 songs, 191 of 191 blocks free." While the page reads, the status line on the FM-1 shows `USB`
   for a moment and the sound pauses.
4. **Save a song in LSDj.** PROJECT (SELECT + UP), LOAD/SAVE SONG, SAVE, choose slot 00, OK. Press Refresh on
   the page. Expected: slot 00 in the list.
5. **Download it.** Press Download. Expected: a `.lsdprj` file in Downloads. If you have LSDPatcher: it opens there.
6. **Upload it back.** Drop that file on the page. Expected: "Uploaded ...", LSDj starts again by itself, the list
   shows two songs (slots 00 and 01).
7. **Load it in LSDj.** PROJECT, LOAD/SAVE SONG, LOAD, slot 01. Press START. Expected: the same song plays.
8. **A song from elsewhere.** Drop any `.lsdsng` or `.lsdprj` you have. Expected: it uploads if its kits are
   LSDj's own; a song with custom kits is refused with their number (custom kits arrive with saving to flash).
9. **The whole save.** Press "Download the whole save (.sav)". Expected: a 131,072-byte file after a few seconds.
   Power the FM-1 off and on, press PLAY, connect again, "Restore a save…", choose that file. Expected: your songs
   are back, the edited song too.
10. **Unplug in the middle** (optional): start a save restore and pull the USB cable. Expected: the page says the
    FM-1 was disconnected; after about 5 seconds LSDj runs again.
11. **The way back.** The installer page still installs (try it once more), and OCT- + OCT+ held 5 s enters UBOOT
    (no countdown in this build).

Tell me what each step did. Useful extras from the console (`screen /dev/cu.usbmodem*`): `ble info`, `ble`.
```

- [ ] **Step 4: Commit**

```bash
git status --short
git add docs/lsdj/DEVICE_STEPS_TRANSFER.md
git commit -m "lsdj: device check sheet for song transfer"
git log --oneline lsdj-emu..HEAD
```

Expected: the spec and plan commits, then one commit for each of Tasks 1 to 7; `git status --short` prints nothing.

- [ ] **Step 5: Hand over**

Report to the owner: the firmware path, the two page addresses, the check sheet path, and the measured numbers from the build's last lines (image size, pool use). Do not merge and do not push.
