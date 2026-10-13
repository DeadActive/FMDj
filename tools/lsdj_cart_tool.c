/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The Mac build's maker of the cartridge block (firmware/src/lsdj/lsdj_cart.c; the install page runs the same code
 * as WebAssembly): the owner's own ROM and a start-up save -> the block, never stored in this repo.
 *   lsdj_cart_tool ROM SAVE OUT MAX      MAX: the bytes free in the app's flash slot
 * Prints the block's size. Exit 1 with the reason if the file is not an LSDj ROM or the block does not fit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lsdj/lsdj_gb.c"
#define LSDJ_CART_MAKE 1
#include "lsdj/lsdj_cart.c"

static uint8_t rom[LSDJ_CART_BANKS * LSDJ_BANK + 1u], save[LSDJ_CART_SAVE + 1u];
static uint8_t out[LSDJ_CART_BANKS * LSDJ_BANK + LSDJ_CART_SAVE + 4096u];

int main(int argc, char **argv)
{
    FILE *f;
    uint32_t n, max;
    int32_t r;
    if (argc != 5) {
        fprintf(stderr, "usage: lsdj_cart_tool ROM SAVE OUT MAX\n");
        return 2;
    }
    if (!(f = fopen(argv[1], "rb")))
        return perror(argv[1]), 2;
    n = (uint32_t)fread(rom, 1, sizeof rom, f);
    fclose(f);
    if (!(f = fopen(argv[2], "rb")))
        return perror(argv[2]), 2;
    if (fread(save, 1, sizeof save, f) != LSDJ_CART_SAVE) {
        fprintf(stderr, "lsdj_cart_tool: %s is not a 128 KiB save\n", argv[2]);
        return 1;
    }
    fclose(f);
    max = (uint32_t)strtoul(argv[4], 0, 0);
    r = lsdj_cart_make(out, max < sizeof out ? max : (uint32_t)sizeof out, rom, n, save);
    if (r < 0) {
        if (r == LSDJ_CART_FULL)
            fprintf(stderr, "lsdj_cart_tool: the ROM needs %u B and %u B are free: %u B too many\n",
                    lsdj_cart_need(rom, n, save), max, lsdj_cart_need(rom, n, save) - max);
        else
            fprintf(stderr, "lsdj_cart_tool: not an LSDj ROM: %s\n",
                    r == LSDJ_CART_ROM_SIZE ? "not 1 .. 64 whole banks of 16 KiB" :
                    r == LSDJ_CART_ROM_TYPE ? "not MBC5 + RAM + battery with a 128 KiB save" : "the header checksum is wrong");
        return 1;
    }
    if (!(f = fopen(argv[3], "wb")) || fwrite(out, 1, (size_t)r, f) != (size_t)r)
        return perror(argv[3]), 2;
    fclose(f);
    printf("cartridge block %d B of %u free (%u banks)\n", r, max, n / LSDJ_BANK);
    return 0;
}
