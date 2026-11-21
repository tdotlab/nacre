/* The cartridge: header checks and the banner title. */

#include "nds_internal.h"

#include <stdio.h>
#include <string.h>

/* Fixed checksum of the Nintendo logo at 0xC0..0x15B, which the firmware checks. */
#define LOGO_CRC 0xCF56u

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

uint16_t nds_crc16(uint16_t crc, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

int nds_rom_looks_valid(const uint8_t *rom, size_t length) {
    if (!rom || length < 0x160) return 0;
    if (le16(rom + 0x15C) != LOGO_CRC) return 0;
    if (nds_crc16(0xFFFF, rom + 0xC0, 0x9C) != LOGO_CRC) return 0;
    return nds_crc16(0xFFFF, rom, 0x15E) == le16(rom + 0x15E);
}

static void copy_text(char *out, const uint8_t *in, size_t length) {
    size_t n = 0;
    for (size_t i = 0; i < length && in[i]; i++)
        out[n++] = (in[i] >= 0x20 && in[i] < 0x7F) ? (char)in[i] : '?';
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

/* A binary must lie inside the file and land where direct boot can put it. */
static int binary_fits(size_t length, uint32_t offset, uint32_t size, uint32_t address,
                       uint32_t entry, int arm7) {
    uint64_t end = (uint64_t)address + size;
    int in_main = address >= NDS_MAIN_RAM_BASE && end <= NDS_MAIN_RAM_BASE + NDS_MAIN_RAM_SIZE;
    int in_wram = arm7 && address >= NDS_ARM7_WRAM_BASE && end <= NDS_ARM7_WRAM_END;
    if (size == 0 || (uint64_t)offset + size > length) return 0;
    if (!in_main && !in_wram) return 0;
    return entry >= address && entry < end;
}

int nds_parse_header(const uint8_t *rom, size_t length, NdsHeader *header, char *error,
                     size_t error_capacity) {
    NdsHeader h;
    if (!rom || length < NDS_HEADER_SIZE) {
        snprintf(error, error_capacity, "This file is too small to be a DS cartridge.");
        return 0;
    }
    if (!nds_rom_looks_valid(rom, length)) {
        snprintf(error, error_capacity, "This file doesn't have a valid DS cartridge header.");
        return 0;
    }
    if (length > NDS_MAX_ROM) {
        snprintf(error, error_capacity, "This cartridge is larger than 512 MiB.");
        return 0;
    }
    memset(&h, 0, sizeof h);
    copy_text(h.title, rom, 12);
    copy_text(h.game_code, rom + 0x0C, 4);
    copy_text(h.maker, rom + 0x10, 2);
    h.unit_code = rom[0x12];
    h.capacity = rom[0x14];
    h.arm9_offset = le32(rom + 0x20);
    h.arm9_entry = le32(rom + 0x24);
    h.arm9_address = le32(rom + 0x28);
    h.arm9_size = le32(rom + 0x2C);
    h.arm7_offset = le32(rom + 0x30);
    h.arm7_entry = le32(rom + 0x34);
    h.arm7_address = le32(rom + 0x38);
    h.arm7_size = le32(rom + 0x3C);
    h.banner_offset = le32(rom + 0x68);
    h.rom_size = le32(rom + 0x80);
    if (h.unit_code == 0x03) {
        snprintf(error, error_capacity, "This is a DSi-only cartridge; Nacre runs DS games.");
        return 0;
    }
    if (!binary_fits(length, h.arm9_offset, h.arm9_size, h.arm9_address, h.arm9_entry, 0)) {
        snprintf(error, error_capacity, "The cartridge's ARM9 program doesn't fit in memory.");
        return 0;
    }
    if (!binary_fits(length, h.arm7_offset, h.arm7_size, h.arm7_address, h.arm7_entry, 1)) {
        snprintf(error, error_capacity, "The cartridge's ARM7 program doesn't fit in memory.");
        return 0;
    }
    *header = h;
    return 1;
}

/* Appends one code point as UTF-8 if it fits. */
static size_t put_utf8(char *out, size_t used, size_t capacity, uint32_t c) {
    uint8_t buf[3];
    size_t n;
    if (c < 0x80) {
        buf[0] = (uint8_t)c;
        n = 1;
    } else if (c < 0x800) {
        buf[0] = (uint8_t)(0xC0 | c >> 6);
        buf[1] = (uint8_t)(0x80 | (c & 0x3F));
        n = 2;
    } else {
        buf[0] = (uint8_t)(0xE0 | c >> 12);
        buf[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (uint8_t)(0x80 | (c & 0x3F));
        n = 3;
    }
    if (used + n >= capacity) return used;
    memcpy(out + used, buf, n);
    return used + n;
}

/* The banner holds each title as up to 128 UTF-16 characters, lines split by
 * newlines; the first line is the game's name. */
void nds_banner_title(const uint8_t *rom, size_t length, const NdsHeader *header, char *out,
                      size_t capacity) {
    const uint32_t english = 0x340, chars = 128;
    size_t used = 0;
    out[0] = '\0';
    if (!header->banner_offset || (uint64_t)header->banner_offset + english + chars * 2 > length) return;
    for (uint32_t i = 0; i < chars; i++) {
        uint16_t c = le16(rom + header->banner_offset + english + i * 2);
        if (c == 0 || c == '\n') break;
        if (c >= 0xD800 && c < 0xE000) c = '?'; /* the banner font has no surrogates */
        if (c < 0x20) continue;
        used = put_utf8(out, used, capacity, c);
    }
    while (used && out[used - 1] == ' ') used--;
    out[used] = '\0';
}
