/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The install page's WebAssembly (web/lsdj_wasm.sh -> build/web/lsdj_emu.wasm): the firmware's own emulator
 * (firmware/src/lsdj/lsdj_emu.c) and the maker of the cartridge block (lsdj_cart.c), so that the browser runs the
 * user's ROM and makes the block exactly as the Mac build does (tools/lsdj_rom.py with tests/lsdj/lsdj_host.c and
 * tools/lsdj_cart_tool.c): the same C code, the same bytes. Nothing of LSDj is in here: the page writes the user's
 * ROM into lw_rom().
 *   lw_rom(), lw_sram(), lw_fb(), lw_out()   the buffers: the ROM file (up to 1 MiB), the 128 KiB save, the picture
 *                                            (160 x 144 shades 0..3), the block
 *   lw_check(len)                            is the file an LSDj ROM: lsdj_cart.c's LSDJ_CART_OK / _ROM_*
 *   lw_start(len)                            the Game Boy from its reset with that ROM and the save in lw_sram():
 *                                            0, or emu_init's error
 *   lw_frames(n, pad)                        n frames with these keys held (Peanut-GB's joypad byte: 0 = down)
 *   lw_errors(), lw_peak(), lw_ink()         since lw_start: emulator errors, the loudest sample, and now: the
 *                                            picture's pixels that differ from its first (0: a blank screen)
 *   lw_need(), lw_cart(max)                  the block's size for the ROM and the save in lw_sram(); the block into
 *                                            lw_out(): its size, or LSDJ_CART_FULL */
#include <stdint.h>
#include <stdlib.h>                           /* (before lsdj_emu.c: it has its own abort) */
#include <string.h>
#define LSDJ_HOST 1
#include <time.h>
#include "lsdj/lsdj_gb.c"
#include "lsdj/lsdj_emu.c"
#define LSDJ_CART_MAKE 1
#include "lsdj/lsdj_cart.c"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define LW EMSCRIPTEN_KEEPALIVE
#else
#define LW                                    /* (the host test builds it natively: tests/lsdj/lsdj_wasm_test) */
#endif

static uint8_t lw_rom_b[LSDJ_CART_BANKS * LSDJ_BANK], lw_sram_b[LSDJ_SRAM], lw_fb_b[LSDJ_H][LSDJ_W];
static uint8_t lw_out_b[LSDJ_CART_BANKS * LSDJ_BANK + LSDJ_CART_SAVE + 4096u];
static lsdj_bank_t lw_banks[LSDJ_CART_BANKS];
static uint32_t lw_len, lw_pk;

LW uint8_t *lw_rom(void) { return lw_rom_b; }
LW uint8_t *lw_sram(void) { return lw_sram_b; }
LW uint8_t *lw_fb(void) { return lw_fb_b[0]; }
LW uint8_t *lw_out(void) { return lw_out_b; }

LW int lw_check(uint32_t len)
{
    return len > sizeof lw_rom_b ? LSDJ_CART_ROM_SIZE : lsdj_cart_rom_check(lw_rom_b, len);
}

LW int lw_start(uint32_t len)
{
    uint32_t i, nb;
    if (len > sizeof lw_rom_b)
        return LSDJ_CART_ROM_SIZE;
    lw_len = len;
    memset(lw_rom_b + len, 0xFF, sizeof lw_rom_b - len);
    nb = (len + LSDJ_BANK - 1u) / LSDJ_BANK;
    for (i = 0; i < nb; i++) {                /* the banks as the block stores them, and the firmware reads them */
        lw_banks[i].p = lw_rom_b + i * LSDJ_BANK;
        lw_banks[i].n = (uint16_t)lsdj_cart_trim(lw_rom_b + i * LSDJ_BANK);
    }
    lw_pk = 0;
    memset(lw_fb_b, 0, sizeof lw_fb_b);
    return emu_init(lw_banks, nb, lw_sram_b, lw_fb_b);
}

LW void lw_frames(uint32_t n, uint32_t pad)
{
    uint32_t s;
    while (n--) {
        emu_frame((uint8_t)pad);
        for (s = 0; s < AUDIO_SAMPLES_TOTAL; s++) {
            uint32_t a = (uint32_t)(emu.snd[s] < 0 ? -emu.snd[s] : emu.snd[s]);
            lw_pk = a > lw_pk ? a : lw_pk;
        }
    }
}

LW uint32_t lw_errors(void) { return emu.errors; }
LW uint32_t lw_peak(void) { return lw_pk; }
LW uint32_t lw_ink(void)
{
    uint32_t i, ink = 0;
    for (i = 0; i < LSDJ_H * LSDJ_W; i++)
        ink += lw_fb_b[0][i] != lw_fb_b[0][0];
    return ink;
}

LW uint32_t lw_need(void) { return lsdj_cart_need(lw_rom_b, lw_len, lw_sram_b); }
LW int32_t lw_cart(uint32_t max)
{
    return lsdj_cart_make(lw_out_b, max < sizeof lw_out_b ? max : (uint32_t)sizeof lw_out_b, lw_rom_b, lw_len, lw_sram_b);
}
