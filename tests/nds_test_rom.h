#ifndef NACRE_NDS_TEST_ROM_H
#define NACRE_NDS_TEST_ROM_H

/* Builds a small synthetic DS cartridge for tests. It carries no Nintendo data: the
 * logo field is filler whose last two bytes are chosen so its checksum comes out to
 * the fixed value the header check wants. */

#include "nds.h"

#include <stdint.h>
#include <string.h>

#define NDS_TEST_ROM_SIZE 0x8000u
#define NDS_TEST_BANNER 0x6000u

static void nds_test_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Recomputes the header checksum after any change to the first 0x15E bytes. */
static void nds_test_seal(uint8_t *rom) {
    uint16_t crc = nds_crc16(0xFFFF, rom, 0x15E);
    rom[0x15E] = (uint8_t)crc;
    rom[0x15F] = (uint8_t)(crc >> 8);
}

/* `banner_title` is ASCII, or NULL for no banner. */
static void nds_test_make_rom(uint8_t *rom, const char *banner_title) {
    memset(rom, 0, NDS_TEST_ROM_SIZE);
    memcpy(rom, "NACRETEST", 9);
    memcpy(rom + 0x0C, "NTST", 4);
    memcpy(rom + 0x10, "01", 2);
    rom[0x14] = 0; /* 128 KiB chip */
    /* ARM9 program: 4 KiB at the start of main RAM. */
    nds_test_put32(rom + 0x20, 0x4000);
    nds_test_put32(rom + 0x24, 0x02000000);
    nds_test_put32(rom + 0x28, 0x02000000);
    nds_test_put32(rom + 0x2C, 0x1000);
    /* ARM7 program: 4 KiB in the ARM7's RAM. */
    nds_test_put32(rom + 0x30, 0x5000);
    nds_test_put32(rom + 0x34, 0x037F8000);
    nds_test_put32(rom + 0x38, 0x037F8000);
    nds_test_put32(rom + 0x3C, 0x1000);
    nds_test_put32(rom + 0x80, NDS_TEST_ROM_SIZE);
    if (banner_title) {
        nds_test_put32(rom + 0x68, NDS_TEST_BANNER);
        rom[NDS_TEST_BANNER] = 1; /* banner version */
        for (size_t i = 0; banner_title[i] && i < 128; i++)
            rom[NDS_TEST_BANNER + 0x340 + i * 2] = (uint8_t)banner_title[i];
    }
    /* Logo field: filler, then the two bytes that bring its checksum to 0xCF56. */
    for (unsigned i = 0; i < 0x9A; i++) rom[0xC0 + i] = (uint8_t)(i * 7 + 3);
    for (unsigned v = 0; v < 0x10000u; v++) {
        rom[0x15A] = (uint8_t)v;
        rom[0x15B] = (uint8_t)(v >> 8);
        if (nds_crc16(0xFFFF, rom + 0xC0, 0x9C) == 0xCF56) break;
    }
    rom[0x15C] = 0x56;
    rom[0x15D] = 0xCF;
    nds_test_seal(rom);
}

#endif
