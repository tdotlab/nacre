/* The cartridge: header checks, the banner title, and the cart bus. */

#include "nds_internal.h"

#include <stdio.h>
#include <string.h>

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
    /* Retail carts carry the Nintendo logo. Homebrew built for flashcards may
     * carry another one, but its stored checksum still matches its contents. */
    if (nds_crc16(0xFFFF, rom + 0xC0, 0x9C) != le16(rom + 0x15C)) return 0;
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

/* ---- The cartridge bus in data mode (GBATEK, "DS Cartridge I/O Ports" and
 * "DS Cartridge Protocol") ----
 *
 * Direct boot leaves the cart in main data mode with KEY2 off as far as the
 * game can tell, so commands and data travel in the clear. A block's bytes are
 * made on demand from the ROM image; the timing only decides when the block
 * counts as finished, which is when cart DMA runs and the IRQ fires. */

#define ROMCTRL_DRQ 0x00800000u
#define ROMCTRL_BUSY 0x80000000u

/* The chip's size: the header's capacity, or the image if that is larger,
 * rounded up to a power of two. Reads mirror at this size. */
static uint32_t chip_size(const Nds *nds) {
    uint64_t size = 0x20000u, want;
    unsigned capacity = nds->header.capacity;
    want = capacity < 13 ? (uint64_t)0x20000u << capacity : NDS_MAX_ROM;
    if (nds->rom_size > want) want = nds->rom_size;
    if (want > NDS_MAX_ROM) want = NDS_MAX_ROM;
    while (size < want) size <<= 1;
    return (uint32_t)size;
}

/* Byte 1 is the size in MiB minus one up to 128 MiB, and counts down in
 * 256 MiB units above that; bit 31 marks the newer, bigger chips, and bit 30
 * the carts made for DSi-enhanced or DSi-only games (GBATEK's list of known
 * IDs), which some of those games check against their header at boot. */
uint32_t nds_cart_chip_id(const Nds *nds) {
    uint32_t mb, size_byte, id;
    if (!nds->rom) return 0xFFFFFFFFu;
    mb = chip_size(nds) >> 20;
    if (mb == 0) mb = 1;
    size_byte = mb <= 128 ? mb - 1 : 0x100u - (mb >> 8);
    id = 0xC2u | (size_byte & 0xFF) << 8;
    if (mb >= 128) id |= 0x80000000u;
    if (nds->header.unit_code & 2) id |= 0x40000000u;
    return id;
}

void nds_cart_reset(Nds *nds) {
    memset(&nds->cart, 0, sizeof nds->cart);
    nds->cart.auxspidata = 0xFF;
}

static uint8_t rom_byte(const Nds *nds, uint32_t a) {
    a &= chip_size(nds) - 1;
    return a < nds->rom_size ? nds->rom[a] : 0xFF;
}

/* Byte `k` of the current block. */
static uint8_t block_byte(const Nds *nds, uint32_t k) {
    const NdsCart *c = &nds->cart;
    if (!nds->rom) return 0xFF;
    switch (c->command[0]) {
    case 0xB7: /* data read: stays inside its 4 KiB block */
        return rom_byte(nds, (c->address & ~0xFFFu) | ((c->address + k) & 0xFFFu));
    case 0x00: /* header, repeated */
        return rom_byte(nds, k & 0xFFFu);
    case 0x90:
    case 0xB8: /* chip ID, repeated every 4 bytes */
        return (uint8_t)(nds_cart_chip_id(nds) >> ((k & 3) * 8));
    default:
        return 0xFF;
    }
}

static void finish(Nds *nds) {
    NdsCart *c = &nds->cart;
    if (!(c->romctrl & ROMCTRL_BUSY)) return;
    c->romctrl &= ~(ROMCTRL_BUSY | ROMCTRL_DRQ);
    c->pending = 0;
    if (c->auxspicnt & 0x4000) nds_irq_raise(nds, nds_cart_owner(nds), NDS_IRQ_CART_DONE);
}

static void start(Nds *nds, uint64_t now) {
    NdsCart *c = &nds->cart;
    uint32_t bs = (c->romctrl >> 24) & 7, clk = (c->romctrl & (1u << 27)) ? 8u : 5u;
    uint32_t gap1 = c->romctrl & 0x1FFF, gap2 = (c->romctrl >> 16) & 0x3F;
    c->length = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    c->pos = 0;
    c->address = (uint32_t)c->command[1] << 24 | (uint32_t)c->command[2] << 16 |
                 (uint32_t)c->command[3] << 8 | c->command[4];
    /* B7 cannot reach the secure area: those addresses fold onto 8000h. */
    if (c->command[0] == 0xB7 && c->address < 0x8000u) c->address = 0x8000u + (c->address & 0x1FF);
    /* Eight command bytes, the leading gap, the data, and a gap2 between each
     * 200h-byte piece, at one clock per byte. */
    c->ready = now + (uint64_t)clk * (8u + gap1 + c->length + (c->length >> 9) * gap2);
    c->pending = 1;
    c->romctrl |= ROMCTRL_BUSY;
    if (c->length) c->romctrl |= ROMCTRL_DRQ;
}

void nds_cart_update(Nds *nds, uint64_t now) {
    NdsCart *c = &nds->cart;
    int owner = nds_cart_owner(nds);
    if (!(c->romctrl & ROMCTRL_BUSY)) return;
    if (c->pending) {
        if (now < c->ready) return;
        c->pending = 0;
    }
    /* Cart DMA moves whatever is left, a block per data request. */
    for (uint32_t guard = c->length / 4 + 1;
         guard && c->pos < c->length && nds_dma_waiting(nds, owner, NDS_DMA_CART); guard--)
        nds_dma_trigger(nds, owner, NDS_DMA_CART);
    if (c->pos >= c->length) finish(nds);
}

uint32_t nds_cart_read_data(Nds *nds, int cpu) {
    NdsCart *c = &nds->cart;
    uint32_t v = 0;
    if (cpu != nds_cart_owner(nds) || !(c->romctrl & ROMCTRL_BUSY) || c->pos >= c->length)
        return 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) v |= (uint32_t)block_byte(nds, c->pos + (uint32_t)i) << (i * 8);
    c->pos += 4;
    if (c->pos >= c->length) {
        c->romctrl &= ~ROMCTRL_DRQ;
        nds_cart_update(nds, nds_now(nds, cpu));
    }
    return v;
}

/* AUXSPICNT and AUXSPIDATA at 1A0h, ROMCTRL at 1A4h, the command at 1A8h. The
 * KEY2 seeds at 1B0h-1BBh are write-only. */
uint32_t nds_cart_read(Nds *nds, int cpu, uint32_t off) {
    const NdsCart *c = &nds->cart;
    if (cpu != nds_cart_owner(nds)) return 0;
    switch (off) {
    case 0x1A0: return c->auxspicnt | (uint32_t)c->auxspidata << 16;
    case 0x1A4: return c->romctrl;
    default: return 0;
    }
}

void nds_cart_write(Nds *nds, int cpu, uint32_t off, uint32_t v, uint32_t mask) {
    NdsCart *c = &nds->cart;
    if (cpu != nds_cart_owner(nds)) return;
    switch (off) {
    case 0x1A0:
        c->auxspicnt = (uint16_t)((c->auxspicnt & ~(mask & 0xE043u)) | (v & mask & 0xE043u));
        /* The backup chip arrives with M5; until then nothing answers. */
        if (mask & 0x00FF0000u) c->auxspidata = 0xFF;
        break;
    case 0x1A4: {
        /* Busy and DRQ are status, bit 15 is a write-only strobe, and RESB
         * (bit 29) stays set once released. */
        uint32_t w = mask & ~(ROMCTRL_BUSY | ROMCTRL_DRQ | (1u << 15));
        c->romctrl = (c->romctrl & ~w) | (v & w) | (c->romctrl & (1u << 29));
        if ((mask & ROMCTRL_BUSY) && (v & ROMCTRL_BUSY)) start(nds, nds_now(nds, cpu));
        break;
    }
    case 0x1A8:
    case 0x1AC:
        for (int i = 0; i < 4; i++)
            if ((mask >> (i * 8)) & 0xFF) c->command[off - 0x1A8 + (uint32_t)i] = (uint8_t)(v >> (i * 8));
        break;
    default:
        break;
    }
}
