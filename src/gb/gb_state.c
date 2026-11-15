/* Save states: a snapshot of the whole machine that can be restored later.
 *
 * Layout: a 20-byte header, then the machine struct copied as-is (cartridge
 * pointers zeroed), then the cartridge RAM. The struct copy keeps this file free
 * of per-field bookkeeping, at the cost of states being valid only for the same
 * build on the same kind of machine: the header records sizeof(Gb), and a state
 * from a different layout is rejected rather than misread. The ROM is not stored;
 * a hash of it ties a state to the game it came from. */

#include "gb_internal.h"

#include <stddef.h>
#include <string.h>

#define STATE_MAGIC "NGBS"
#define STATE_VERSION 1u
#define STATE_HEADER 20u

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* FNV-1a over the whole ROM. */
static uint32_t rom_hash(const GbCart *cart) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < cart->rom_size; i++) {
        h ^= cart->rom[i];
        h *= 16777619u;
    }
    return h;
}

size_t gb_state_size(const Gb *gb) {
    if (!gb || !gb->cart.rom) return 0;
    return STATE_HEADER + sizeof(Gb) + gb->cart.ram_size;
}

size_t gb_state_save(const Gb *gb, uint8_t *out, size_t capacity) {
    size_t size = gb_state_size(gb);
    if (size == 0 || !out || capacity < size) return 0;
    memcpy(out, STATE_MAGIC, 4);
    put32(out + 4, STATE_VERSION);
    put32(out + 8, (uint32_t)sizeof(Gb));
    put32(out + 12, rom_hash(&gb->cart));
    put32(out + 16, (uint32_t)gb->cart.ram_size);
    memcpy(out + STATE_HEADER, gb, sizeof(Gb));
    /* The pointers mean nothing in another run; the loader keeps its own. */
    memset(out + STATE_HEADER + offsetof(Gb, cart.rom), 0, sizeof gb->cart.rom);
    memset(out + STATE_HEADER + offsetof(Gb, cart.ram), 0, sizeof gb->cart.ram);
    if (gb->cart.ram_size) memcpy(out + STATE_HEADER + sizeof(Gb), gb->cart.ram, gb->cart.ram_size);
    return size;
}

int gb_state_load(Gb *gb, const uint8_t *data, size_t length) {
    GbCart live;
    if (!gb || !gb->cart.rom || !data) return 0;
    if (length != gb_state_size(gb)) return 0;
    if (memcmp(data, STATE_MAGIC, 4) != 0) return 0;
    if (get32(data + 4) != STATE_VERSION) return 0;
    if (get32(data + 8) != (uint32_t)sizeof(Gb)) return 0;
    if (get32(data + 12) != rom_hash(&gb->cart)) return 0;
    if (get32(data + 16) != (uint32_t)gb->cart.ram_size) return 0;

    live = gb->cart;
    memcpy(gb, data + STATE_HEADER, sizeof(Gb));
    gb->cart.rom = live.rom;
    gb->cart.ram = live.ram;
    gb->cart.wall_clock = live.wall_clock; /* the frontend's clock, not the state's */
    if (live.ram_size) memcpy(gb->cart.ram, data + STATE_HEADER + sizeof(Gb), live.ram_size);
    gb->cart.dirty = 1; /* cartridge RAM changed, so the battery file needs rewriting */
    /* Audio queued before the load belongs to a different moment. */
    gb->apu.read_pos = 0;
    gb->apu.write_pos = 0;
    return 1;
}
