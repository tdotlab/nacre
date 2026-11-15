/* Cartridge header, memory bank controllers, battery RAM, and the MBC3 clock. */

#include "gb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t nintendo_logo[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
};

int gb_rom_looks_valid(const uint8_t *rom, size_t length) {
    return rom && length >= 0x150 && memcmp(rom + 0x104, nintendo_logo, sizeof nintendo_logo) == 0;
}

static void fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
}

int gb_cart_load(GbCart *cart, const uint8_t *rom, size_t length, char *error, size_t capacity) {
    uint8_t type;
    size_t declared;
    size_t rom_size;
    size_t ram_size;
    int i;

    memset(cart, 0, sizeof *cart);
    if (!rom || length < 0x150) {
        fail(error, capacity, "This file is too small to be a Game Boy cartridge.");
        return 0;
    }
    if (length > 8u * 1024u * 1024u) {
        fail(error, capacity, "This file is larger than any Game Boy cartridge (8 MB).");
        return 0;
    }
    if (rom[0x143] == 0xC0) {
        fail(error, capacity, "This is a Game Boy Color-only game. Nacre emulates the original Game Boy.");
        return 0;
    }

    type = rom[0x147];
    switch (type) {
    case 0x00: cart->mbc = GB_MBC_NONE; break;
    case 0x08: cart->mbc = GB_MBC_NONE; break;
    case 0x09: cart->mbc = GB_MBC_NONE; cart->has_battery = 1; break;
    case 0x01: case 0x02: cart->mbc = GB_MBC1; break;
    case 0x03: cart->mbc = GB_MBC1; cart->has_battery = 1; break;
    case 0x05: cart->mbc = GB_MBC2; break;
    case 0x06: cart->mbc = GB_MBC2; cart->has_battery = 1; break;
    case 0x0F: case 0x10: cart->mbc = GB_MBC3; cart->has_battery = 1; cart->has_rtc = 1; break;
    case 0x11: case 0x12: cart->mbc = GB_MBC3; break;
    case 0x13: cart->mbc = GB_MBC3; cart->has_battery = 1; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: cart->mbc = GB_MBC5; break;
    case 0x1B: case 0x1E: cart->mbc = GB_MBC5; cart->has_battery = 1; break;
    default: {
        char message[128];
        snprintf(message, sizeof message,
                 "This cartridge uses hardware Nacre does not support yet (type $%02X).", type);
        fail(error, capacity, message);
        return 0;
    }
    }

    /* Size the ROM from the header, but never read past the file. */
    declared = rom[0x148] <= 8 ? (size_t)32768u << rom[0x148] : length;
    rom_size = 32768;
    while (rom_size < length || rom_size < declared) rom_size <<= 1;

    switch (rom[0x149]) {
    case 0x02: ram_size = 8 * 1024; break;
    case 0x03: ram_size = 32 * 1024; break;
    case 0x04: ram_size = 128 * 1024; break;
    case 0x05: ram_size = 64 * 1024; break;
    default: ram_size = 0; break;
    }
    if (cart->mbc == GB_MBC2) ram_size = 512;
    if (cart->mbc == GB_MBC_NONE && ram_size > 8192) ram_size = 8192;
    if (cart->mbc == GB_MBC1 && ram_size > 32768) ram_size = 32768;

    cart->rom = (uint8_t *)malloc(rom_size);
    cart->ram = ram_size ? (uint8_t *)malloc(ram_size) : NULL;
    if (!cart->rom || (ram_size && !cart->ram)) {
        free(cart->rom);
        free(cart->ram);
        memset(cart, 0, sizeof *cart);
        fail(error, capacity, "Not enough memory to load this cartridge.");
        return 0;
    }
    /* Unpopulated space reads as open bus. */
    memset(cart->rom, 0xFF, rom_size);
    memcpy(cart->rom, rom, length);
    if (ram_size) memset(cart->ram, 0xFF, ram_size);
    cart->rom_size = rom_size;
    cart->ram_size = ram_size;

    for (i = 0; i < 16; i++) {
        uint8_t ch = rom[0x134 + i];
        if (ch == 0 || ((rom[0x143] & 0x80) && i >= 11)) break;
        cart->title[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : ' ';
    }
    cart->title[i] = '\0';
    while (i > 0 && cart->title[i - 1] == ' ') cart->title[--i] = '\0';

    gb_cart_reset(cart);
    return 1;
}

void gb_cart_free(GbCart *cart) {
    free(cart->rom);
    free(cart->ram);
    cart->rom = NULL;
    cart->ram = NULL;
    cart->rom_size = 0;
    cart->ram_size = 0;
}

void gb_cart_reset(GbCart *cart) {
    cart->ram_enabled = 0;
    cart->rom_bank = 1;
    cart->ram_bank = 0;
    cart->mode = 0;
    cart->latch_prev = 0xFF;
}

static uint8_t rom_at(const GbCart *cart, size_t bank, uint16_t offset) {
    size_t index = ((bank * 0x4000u) + offset) & (cart->rom_size - 1);
    return cart->rom[index];
}

static size_t ram_index(const GbCart *cart, size_t bank, uint16_t address) {
    return ((bank * 0x2000u) + (address - 0xA000u)) % cart->ram_size;
}

static int rtc_selected(const GbCart *cart) {
    return cart->has_rtc && cart->ram_bank >= 0x08 && cart->ram_bank <= 0x0C;
}

static uint8_t *rtc_reg(GbRtcRegs *r, uint8_t select) {
    switch (select) {
    case 0x08: return &r->seconds;
    case 0x09: return &r->minutes;
    case 0x0A: return &r->hours;
    case 0x0B: return &r->days_low;
    default: return &r->days_high;
    }
}

uint8_t gb_cart_read(GbCart *cart, uint16_t a) {
    if (!cart->rom) return 0xFF;
    if (a < 0x4000) {
        size_t bank = 0;
        if (cart->mbc == GB_MBC1 && cart->mode) bank = (size_t)(cart->ram_bank & 3) << 5;
        return rom_at(cart, bank, a);
    }
    if (a < 0x8000) {
        size_t bank = cart->rom_bank;
        if (cart->mbc == GB_MBC1) bank = (size_t)((cart->ram_bank & 3) << 5) | (cart->rom_bank & 0x1F);
        return rom_at(cart, bank, (uint16_t)(a - 0x4000));
    }
    /* A000-BFFF */
    if (!cart->ram_enabled) return 0xFF;
    if (rtc_selected(cart)) {
        static const uint8_t masks[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
        return (uint8_t)(*rtc_reg(&cart->latched, cart->ram_bank) & masks[cart->ram_bank - 0x08]);
    }
    if (!cart->ram_size) return 0xFF;
    switch (cart->mbc) {
    case GB_MBC2:
        return (uint8_t)(0xF0 | (cart->ram[(a - 0xA000) & 0x1FF] & 0x0F));
    case GB_MBC1:
        return cart->ram[ram_index(cart, cart->mode ? (cart->ram_bank & 3) : 0, a)];
    case GB_MBC3:
        if (cart->ram_bank > 3) return 0xFF;
        return cart->ram[ram_index(cart, cart->ram_bank, a)];
    case GB_MBC5:
        return cart->ram[ram_index(cart, cart->ram_bank & 0x0F, a)];
    default:
        return cart->ram[ram_index(cart, 0, a)];
    }
}

void gb_cart_write(GbCart *cart, uint16_t a, uint8_t v) {
    if (!cart->rom) return;
    if (a < 0x8000) {
        switch (cart->mbc) {
        case GB_MBC1:
            if (a < 0x2000) cart->ram_enabled = (v & 0x0F) == 0x0A;
            else if (a < 0x4000) { cart->rom_bank = (uint16_t)(v & 0x1F); if (!cart->rom_bank) cart->rom_bank = 1; }
            else if (a < 0x6000) cart->ram_bank = (uint8_t)(v & 3);
            else cart->mode = (uint8_t)(v & 1);
            break;
        case GB_MBC2:
            if (a < 0x4000) {
                if (a & 0x100) { cart->rom_bank = (uint16_t)(v & 0x0F); if (!cart->rom_bank) cart->rom_bank = 1; }
                else cart->ram_enabled = (v & 0x0F) == 0x0A;
            }
            break;
        case GB_MBC3:
            if (a < 0x2000) cart->ram_enabled = (v & 0x0F) == 0x0A;
            else if (a < 0x4000) { cart->rom_bank = (uint16_t)(v & 0x7F); if (!cart->rom_bank) cart->rom_bank = 1; }
            else if (a < 0x6000) cart->ram_bank = v;
            else {
                if (cart->latch_prev == 0x00 && v == 0x01) cart->latched = cart->rtc;
                cart->latch_prev = v;
            }
            break;
        case GB_MBC5:
            if (a < 0x2000) cart->ram_enabled = (v & 0x0F) == 0x0A;
            else if (a < 0x3000) cart->rom_bank = (uint16_t)((cart->rom_bank & 0x100) | v);
            else if (a < 0x4000) cart->rom_bank = (uint16_t)((cart->rom_bank & 0xFF) | ((v & 1) << 8));
            else if (a < 0x6000) cart->ram_bank = (uint8_t)(v & 0x0F);
            break;
        default:
            break;
        }
        return;
    }
    /* A000-BFFF */
    if (!cart->ram_enabled) return;
    if (rtc_selected(cart)) {
        uint8_t *live = rtc_reg(&cart->rtc, cart->ram_bank);
        if (cart->ram_bank == 0x08) cart->rtc_subsecond = 0;
        *live = v;
        *rtc_reg(&cart->latched, cart->ram_bank) = v;
        cart->dirty = 1;
        return;
    }
    if (!cart->ram_size) return;
    switch (cart->mbc) {
    case GB_MBC2: cart->ram[(a - 0xA000) & 0x1FF] = (uint8_t)(v & 0x0F); break;
    case GB_MBC1: cart->ram[ram_index(cart, cart->mode ? (cart->ram_bank & 3) : 0, a)] = v; break;
    case GB_MBC3: if (cart->ram_bank <= 3) cart->ram[ram_index(cart, cart->ram_bank, a)] = v; break;
    case GB_MBC5: cart->ram[ram_index(cart, cart->ram_bank & 0x0F, a)] = v; break;
    default: cart->ram[ram_index(cart, 0, a)] = v; break;
    }
    cart->dirty = 1;
}

/* ---- MBC3 clock -------------------------------------------------------------- */

static void rtc_add_seconds(GbRtcRegs *r, uint64_t seconds) {
    uint64_t days;
    uint64_t total;
    if (r->days_high & 0x40) return; /* halted */
    total = (uint64_t)(r->seconds % 60) + seconds;
    r->seconds = (uint8_t)(total % 60);
    total = (uint64_t)(r->minutes % 60) + total / 60;
    r->minutes = (uint8_t)(total % 60);
    total = (uint64_t)(r->hours % 24) + total / 60;
    r->hours = (uint8_t)(total % 24);
    days = (uint64_t)(r->days_low | ((r->days_high & 1) << 8)) + total / 24;
    if (days > 511) r->days_high |= 0x80;
    days &= 511;
    r->days_low = (uint8_t)days;
    r->days_high = (uint8_t)((r->days_high & 0xC0) | (days >> 8));
}

void gb_cart_tick(GbCart *cart, int cycles) {
    if (!cart->has_rtc) return;
    cart->rtc_subsecond += (uint32_t)cycles;
    if (cart->rtc_subsecond >= GB_CLOCK_HZ) {
        cart->rtc_subsecond -= GB_CLOCK_HZ;
        rtc_add_seconds(&cart->rtc, 1);
    }
}

/* ---- battery saves -------------------------------------------------------------- */

#define RTC_BLOCK 48

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t gb_save_size(const Gb *gb) {
    const GbCart *cart = &gb->cart;
    if (!cart->has_battery) return 0;
    return cart->ram_size + (cart->has_rtc ? RTC_BLOCK : 0);
}

size_t gb_save_write(const Gb *gb, uint8_t *out, size_t capacity) {
    const GbCart *cart = &gb->cart;
    size_t size = gb_save_size(gb);
    if (!size || capacity < size) return 0;
    if (cart->ram_size) memcpy(out, cart->ram, cart->ram_size);
    if (cart->has_rtc) {
        uint8_t *p = out + cart->ram_size;
        const GbRtcRegs *regs[2] = {&cart->rtc, &cart->latched};
        int set;
        uint64_t stamp = cart->wall_clock > 0 ? (uint64_t)cart->wall_clock : 0;
        for (set = 0; set < 2; set++) {
            put32(p + set * 20 + 0, regs[set]->seconds);
            put32(p + set * 20 + 4, regs[set]->minutes);
            put32(p + set * 20 + 8, regs[set]->hours);
            put32(p + set * 20 + 12, regs[set]->days_low);
            put32(p + set * 20 + 16, regs[set]->days_high);
        }
        put32(p + 40, (uint32_t)stamp);
        put32(p + 44, (uint32_t)(stamp >> 32));
    }
    return size;
}

int gb_save_load(Gb *gb, const uint8_t *data, size_t length) {
    GbCart *cart = &gb->cart;
    if (!cart->has_battery || !data) return 0;
    if (cart->ram_size) {
        if (length < cart->ram_size) return 0;
        memcpy(cart->ram, data, cart->ram_size);
    }
    if (cart->has_rtc && length >= cart->ram_size + RTC_BLOCK) {
        const uint8_t *p = data + cart->ram_size;
        GbRtcRegs *regs[2] = {&cart->rtc, &cart->latched};
        int set;
        uint64_t stamp = (uint64_t)get32(p + 40) | ((uint64_t)get32(p + 44) << 32);
        for (set = 0; set < 2; set++) {
            regs[set]->seconds = (uint8_t)get32(p + set * 20 + 0);
            regs[set]->minutes = (uint8_t)get32(p + set * 20 + 4);
            regs[set]->hours = (uint8_t)get32(p + set * 20 + 8);
            regs[set]->days_low = (uint8_t)get32(p + set * 20 + 12);
            regs[set]->days_high = (uint8_t)get32(p + set * 20 + 16);
        }
        /* The cartridge clock kept running while the game was closed. */
        if (stamp && cart->wall_clock > 0 && (uint64_t)cart->wall_clock > stamp) {
            rtc_add_seconds(&cart->rtc, (uint64_t)cart->wall_clock - stamp);
        }
    }
    cart->dirty = 0;
    return 1;
}

int gb_save_dirty(const Gb *gb) { return gb->cart.dirty; }
void gb_save_mark_clean(Gb *gb) { gb->cart.dirty = 0; }
void gb_set_wall_clock(Gb *gb, int64_t unix_seconds) { gb->cart.wall_clock = unix_seconds; }
