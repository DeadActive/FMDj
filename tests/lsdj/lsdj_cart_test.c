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
    {   /* a block whose CRC is right and whose numbers are not (made by other code, or a later version's): each
         * is refused by its own check, and nothing outside the block is read (the suite builds this test with the
         * address sanitizer) */
        static const struct { uint32_t at, len; uint32_t v; } BAD[] = {
            {2u * NB, 4, 0x7FFFFFF0u},             /* the save's length: past the body, */
            {2u * NB, 4, 0xFFFFFFFFu},
            {0, 2, LSDJ_BANK + 1u},                /* a bank longer than a bank, */
            {2, 2, 4999u},                         /* a bank a byte shorter: the lengths do not add up, */
            {2u * (NB - 1u), 2, 1u}};              /* the last bank a byte longer than the body has */
        uint32_t k, b;
        for (k = 0; k < sizeof BAD / sizeof BAD[0]; k++) {
            uint8_t *body = slot + sizeof slot - need, *t = slot + sizeof slot - LSDJ_CART_TRAILER;
            uint32_t crc;
            place(sizeof blk);
            for (b = 0; b < BAD[k].len; b++)
                body[BAD[k].at + b] = (uint8_t)(BAD[k].v >> (8u * b));
            crc = lsdj_cart_crc(body, need - LSDJ_CART_TRAILER);
            for (b = 0; b < 4u; b++)
                t[12u + b] = (uint8_t)(crc >> (8u * b));
            CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_BAD);
        }
        for (k = 0; k < 2u * NB + 4u; k++) {       /* a body too short for its own table, its CRC right */
            uint8_t *t = slot + sizeof slot - LSDJ_CART_TRAILER;
            uint32_t crc;
            place(sizeof blk);
            crc = lsdj_cart_crc(t - k, k);
            for (b = 0; b < 4u; b++) {
                t[8u + b] = (uint8_t)(k >> (8u * b));
                t[12u + b] = (uint8_t)(crc >> (8u * b));
            }
            CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_BAD);
            CHECK(lsdj_cart_read(end, LSDJ_CART_TRAILER + k, &c) == LSDJ_CART_BAD);    /* (with no byte before it) */
        }
        for (k = 0; k < 2u * LSDJ_CART_BANKS + 4u; k += 3u) {   /* ... and the same saying it has 64 banks: its
                                                                 * table would reach past the end of the slot */
            uint8_t *t = slot + sizeof slot - LSDJ_CART_TRAILER;
            uint32_t crc;
            memset(slot, 0, sizeof slot);
            crc = lsdj_cart_crc(t - k, k);
            memcpy(t, "LSDC\1\0\100\0", 8);
            for (b = 0; b < 4u; b++) {
                t[8u + b] = (uint8_t)(k >> (8u * b));
                t[12u + b] = (uint8_t)(crc >> (8u * b));
            }
            CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_BAD);
        }
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
    {   /* DMGo's kind: its title, MBC5 + rumble + RAM + battery, a 32 KiB save. The block packs 32 KiB (the save
         * size is the ROM header's), and the reader is the same */
        static uint8_t small[32u * 1024u], sback[32u * 1024u];
        uint32_t k;
        uint8_t x = 0;
        memcpy(rom + 0x134, "DRUMMACHINA\0\0\0\0\0", 16);
        rom[0x147] = 0x1E;
        rom[0x149] = 0x03;
        for (k = 0x134; k <= 0x14C; k++)
            x = (uint8_t)(x - rom[k] - 1u);
        rom[0x14D] = x;
        CHECK(lsdj_cart_rom_check(rom, 4u * LSDJ_BANK) == LSDJ_CART_OK);
        for (k = 0; k < sizeof small; k++)
            small[k] = (uint8_t)(k / 300u);
        need = lsdj_cart_need(rom, 4u * LSDJ_BANK, small);
        n = lsdj_cart_make(blk, sizeof blk, rom, 4u * LSDJ_BANK, small);
        CHECK(n > 0 && (uint32_t)n == need);
        memset(slot, 0xFF, sizeof slot);
        memcpy(slot + sizeof slot - (uint32_t)n, blk, (uint32_t)n);
        CHECK(lsdj_cart_read(end, sizeof slot, &c) == LSDJ_CART_OK && c.nbanks == 4u);
        CHECK(lsdj_unpack(sback, sizeof sback, c.save, c.save_len) == 0 && !memcmp(sback, small, sizeof small));
        CHECK(lsdj_unpack(back, sizeof back, c.save, c.save_len) != 0);   /* it is 32 KiB, not 128 */
        rom[0x149] = 0x04;                     /* DMGo's title with a 128 KiB save: not DMGo, not LSDj */
        CHECK(lsdj_cart_rom_check(rom, 4u * LSDJ_BANK) == LSDJ_CART_ROM_TYPE);
    }
    printf(fails ? "lsdj_cart_test: %d FAILED\n" : "lsdj_cart_test: all passed\n", fails);
    return fails != 0;
}
