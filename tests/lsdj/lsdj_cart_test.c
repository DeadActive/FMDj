/* SPDX-License-Identifier: GPL-3.0-only
 * LSDj fork: 2026 DEADACTIVE */
/* The cartridge block (firmware/src/lsdj/lsdj_cart.c): made from a ROM and a save, read back; what is not a block, a
 * damaged one and a ROM that is not LSDj's kind are refused. The ROM here is made up (no LSDj data).
 *   cc -Wall -O1 -Ifirmware/src -o build/tests/lsdj_cart_test tests/lsdj/lsdj_cart_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lsdj/lsdj_gb.c"
#define LSDJ_CART_MAKE 1
#include "lsdj/lsdj_cart.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define NB 6u
static uint8_t rom[NB * LSDJ_BANK], save[LSDJ_CART_SAVE], back[LSDJ_CART_SAVE];
static uint8_t slot[NB * LSDJ_BANK + LSDJ_CART_SAVE + 4096u], blk[sizeof slot];
static uint32_t rs = 7u;
static uint32_t rnd(void) { return (rs = rs * 1664525u + 1013904223u) >> 8; }

static void make_rom(void)
{
    uint32_t i;
    uint8_t x = 0;
    for (i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)rnd();
    memset(rom + 1u * LSDJ_BANK + 5000u, 0xFF, LSDJ_BANK - 5000u);   /* bank 1: 5000 bytes, */
    rom[1u * LSDJ_BANK + 4999u] = 0x12;
    memset(rom + 2u * LSDJ_BANK, 0xFF, LSDJ_BANK);                   /* bank 2: empty, */
    rom[3u * LSDJ_BANK + LSDJ_BANK - 1u] = 0x34;                     /* bank 3: full, */
    memset(rom + 4u * LSDJ_BANK + 1u, 0xFF, LSDJ_BANK - 1u);         /* bank 4: one byte, */
    rom[4u * LSDJ_BANK] = 0x56;
    memset(rom + 5u * LSDJ_BANK, 0xFF, LSDJ_BANK);                   /* the last: empty */
    rom[0x147] = 0x1B;
    rom[0x149] = 0x04;
    for (i = 0x134; i <= 0x14C; i++)
        x = (uint8_t)(x - rom[i] - 1u);
    rom[0x14D] = x;
    for (i = 0; i < sizeof save; i++)         /* a save: runs of every length and plain bytes */
        save[i] = (uint8_t)((i / 700u) & 1u ? rnd() : i / 97u);
    memset(save + 50000u, 0, 30000u);
}

static int32_t place(uint32_t max)             /* the block made, and put at the end of the slot */
{
    int32_t n = lsdj_cart_make(blk, max, rom, sizeof rom, save);
    memset(slot, 0xFF, sizeof slot);
    if (n > 0)
        memcpy(slot + sizeof slot - (uint32_t)n, blk, (uint32_t)n);
    return n;
}

int main(void)
{
    static const uint32_t LEN[NB] = {LSDJ_BANK, 5000u, 0, LSDJ_BANK, 1u, 0};
    lsdj_cart_t c;
    const uint8_t *end = slot + sizeof slot;
    uint32_t i, need;
    int32_t n;
    make_rom();
    CHECK(lsdj_cart_rom_check(rom, sizeof rom) == LSDJ_CART_OK);
    need = lsdj_cart_need(rom, sizeof rom, save);
    n = place(sizeof blk);
    CHECK(n > 0 && (uint32_t)n == need);
    CHECK(need < 2u * LSDJ_BANK + 5001u + 2u * NB + 4u + LSDJ_CART_SAVE + LSDJ_CART_TRAILER);   /* (the save packs) */
    memset(&c, 0x55, sizeof c);
    CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_OK);
    CHECK(c.nbanks == NB);
    for (i = 0; i < NB; i++) {                 /* every bank as lsdj_rom_byte wants it */
        CHECK(c.bank[i].n == LEN[i]);
        CHECK(!LEN[i] ? c.bank[i].p == 0 : !memcmp(c.bank[i].p, rom + i * LSDJ_BANK, LEN[i]));
    }
    for (i = NB; i < LSDJ_CART_BANKS; i++)
        CHECK(c.bank[i].p == 0 && c.bank[i].n == 0);
    for (i = 0; i < sizeof rom; i += 37u)
        CHECK(lsdj_rom_byte(c.bank, c.nbanks, i) == rom[i]);
    CHECK(lsdj_unpack(back, sizeof back, c.save, c.save_len) == 0 && !memcmp(back, save, sizeof save));
    CHECK(lsdj_cart_read(end, need, &c) == LSDJ_CART_OK);            /* with exactly its room */
    CHECK(lsdj_cart_read(end, need - 1u, &c) == LSDJ_CART_BAD);      /* ... not with a byte less */

    {   /* what is not a block */
        memset(slot, 0xFF, sizeof slot);
        CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_NONE);   /* an empty slot: the firmware alone */
        CHECK(lsdj_cart_read(end, 8, &c) == LSDJ_CART_NONE);
        place(sizeof blk);
        slot[sizeof slot - 16u] = 'X';
        CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_NONE);
    }
    for (i = 0; i < 400u; i++) {               /* a damaged one: any one bit, in the trailer or the body */
        uint32_t at = i < 96u ? sizeof slot - 12u + i / 8u : sizeof slot - need + rnd() % (need - 16u);
        place(sizeof blk);
        slot[at] ^= (uint8_t)(1u << (i & 7u));
        CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_BAD);
    }
    {   /* another version, another count of banks: the trailer is not in the CRC, so each is checked itself */
        place(sizeof blk);
        slot[sizeof slot - 12u] = 2;
        CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_BAD);
        place(sizeof blk);
        slot[sizeof slot - 10u] = 0;
        CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_BAD);
        place(sizeof blk);
        slot[sizeof slot - 10u] = 65;
        CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_BAD);
    }
    {   /* the room: one byte short is refused and nothing is written past it */
        memset(blk, 0xA5, sizeof blk);
        CHECK(lsdj_cart_make(blk, need - 1u, rom, sizeof rom, save) == LSDJ_CART_FULL);
        CHECK(lsdj_cart_make(blk, need, rom, sizeof rom, save) == (int32_t)need && blk[need] == 0xA5);
    }
    {   /* a file that is not an LSDj ROM */
        CHECK(lsdj_cart_rom_check(rom, 0) == LSDJ_CART_ROM_SIZE);
        CHECK(lsdj_cart_rom_check(rom, LSDJ_BANK + 1u) == LSDJ_CART_ROM_SIZE);
        CHECK(lsdj_cart_rom_check(rom, 65u * LSDJ_BANK) == LSDJ_CART_ROM_SIZE);
        rom[0x147] = 0x19;                     /* MBC5 without RAM */
        CHECK(lsdj_cart_rom_check(rom, sizeof rom) == LSDJ_CART_ROM_TYPE);
        CHECK(lsdj_cart_make(blk, sizeof blk, rom, sizeof rom, save) == LSDJ_CART_ROM_TYPE);
        rom[0x147] = 0x1B;
        rom[0x149] = 0x03;                     /* a 32 KiB save */
        CHECK(lsdj_cart_rom_check(rom, sizeof rom) == LSDJ_CART_ROM_TYPE);
        rom[0x149] = 0x04;
        rom[0x140] ^= 1;
        CHECK(lsdj_cart_rom_check(rom, sizeof rom) == LSDJ_CART_ROM_SUM);
        rom[0x140] ^= 1;
        CHECK(lsdj_cart_rom_check(rom, sizeof rom) == LSDJ_CART_OK);
    }
    printf(fails ? "lsdj_cart_test: %d FAILED\n" : "lsdj_cart_test: all passed\n", fails);
    return fails != 0;
}
