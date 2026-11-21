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

/* The test program, assembled from tests/nds_test_rom.s. The ARM9 sets both
 * engines' backdrops (red over green), calls SWI Div(100, 7), waits for the
 * ARM7 to send 5 over IPCSYNC, then takes three VBlank IRQs through the BIOS
 * with VBlankIntrWait, then turns IME off. Results land at 0x02100000: quotient, remainder, IRQ
 * count, the IPCSYNC value, and 0x77 when done. The ARM7 sends 5 and halts. */
static const uint32_t nds_test_arm9[] = {
    0xE321F092, 0xE59FD0E0, 0xE321F01F, 0xE59FD0DC, 0xE3A00301, 0xE3A01801,
    0xE5801000, 0xE2804A01, 0xE5841000, 0xE3A02405, 0xE3A0301F, 0xE1C230B0,
    0xE2825B01, 0xE3A03E3E, 0xE1C530B0, 0xE3A06621, 0xE3A00064, 0xE3A01007,
    0xEF090000, 0xE5860000, 0xE5861004, 0xE3A00301, 0xE2807D06, 0xE1D710B0,
    0xE201100F, 0xE3510005, 0x1AFFFFFB, 0xE586100C, 0xE59F107C, 0xE28F2040,
    0xE5812000, 0xE3A01008, 0xE1C010B4, 0xE3A01001, 0xE5801210, 0xE5801208,
    0xE3A08003, 0xEF050000, 0xE2588001, 0x1AFFFFFC, 0xE3A00301, 0xE3A01000,
    0xE5801208, 0xE3A01077, 0xE5861010, 0xEE070F90, 0xEAFFFFFD, 0xE3A00301,
    0xE5901214, 0xE5801214, 0xE59F2028, 0xE5923000, 0xE1833001, 0xE5823000,
    0xE59F201C, 0xE5923000, 0xE2833001, 0xE5823000, 0xE12FFF1E, 0x027C3F00,
    0x027C3E00, 0x027C3FFC, 0x027C3FF8, 0x02100008,
};
static const uint32_t nds_test_arm7[] = {
    0xE3A00301, 0xE3A01C05, 0xE2800D06, 0xE1C010B0, 0xE3A00301, 0xE3A01080,
    0xE5C01301, 0xEAFFFFFD,
};

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
    for (size_t i = 0; i < sizeof nds_test_arm9 / 4; i++) nds_test_put32(rom + 0x4000 + i * 4, nds_test_arm9[i]);
    for (size_t i = 0; i < sizeof nds_test_arm7 / 4; i++) nds_test_put32(rom + 0x5000 + i * 4, nds_test_arm7[i]);
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
