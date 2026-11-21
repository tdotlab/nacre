/* Both CPUs' memory maps (GBATEK, "DS Memory Maps"), the shared WRAM split and
 * VRAM bank mapping ("DS Memory Control"). Plain memory is reached through a
 * pointer lookup; I/O, overlapping VRAM banks and the empty GBA slot take the
 * slow path. */

#include "nds_internal.h"

#include <string.h>

static inline uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t get_sized(const uint8_t *p, int size) {
    return size == 4 ? get32(p) : size == 2 ? get16(p) : p[0];
}
static void put_sized(uint8_t *p, uint32_t v, int size) {
    if (size == 4) put32(p, v);
    else if (size == 2) put16(p, (uint16_t)v);
    else p[0] = (uint8_t)v;
}

/* ---- VRAM banks ---- */

static const uint32_t bank_offset[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000,
                                         0x90000, 0x94000, 0x98000, 0xA0000};
static const uint32_t bank_size[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                       0x4000, 0x4000, 0x8000, 0x4000};
static const unsigned region_pages[NDS_VR_COUNT] = {32, 8, 16, 8, 64};

static void map_pages(NdsVramPage *pages, unsigned count, uint32_t at, uint8_t *bank, uint32_t size) {
    for (uint32_t k = 0; k < size / 0x4000; k++) {
        NdsVramPage *p = &pages[(at / 0x4000 + k) & (count - 1)];
        if (p->count < 4) p->bank[p->count++] = bank + k * 0x4000;
    }
}

void nds_map_vram(Nds *nds) {
    memset(nds->vmap9, 0, sizeof nds->vmap9);
    memset(nds->vmap7, 0, sizeof nds->vmap7);
    for (int b = 0; b < 9; b++) {
        uint8_t cnt = nds->vramcnt[b], *mem = nds->vram + bank_offset[b];
        unsigned mst = cnt & ((b <= 1 || b >= 7) ? 3 : 7), ofs = (cnt >> 3) & 3;
        uint32_t size = bank_size[b];
        int region = -1;
        uint32_t at = 0;
        if (!(cnt & 0x80)) continue;
        if (mst == 0) {
            region = NDS_VR_LCDC;
            at = bank_offset[b];
        } else if (b <= 3) {
            if (mst == 1) {
                region = NDS_VR_ABG;
                at = 0x20000 * ofs;
            } else if (mst == 2 && b <= 1) {
                region = NDS_VR_AOBJ;
                at = 0x20000 * (ofs & 1);
            } else if (mst == 2) {
                map_pages(nds->vmap7, 16, 0x20000 * (ofs & 1), mem, size); /* ARM7 WRAM */
            } else if (mst == 4 && b == 2) {
                region = NDS_VR_BBG;
            } else if (mst == 4 && b == 3) {
                region = NDS_VR_BOBJ;
            }
        } else if (b == 4) {
            if (mst == 1) region = NDS_VR_ABG;
            else if (mst == 2) region = NDS_VR_AOBJ;
        } else if (b <= 6) {
            at = 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
            if (mst == 1) region = NDS_VR_ABG;
            else if (mst == 2) region = NDS_VR_AOBJ;
        } else if (b == 7) {
            if (mst == 1) region = NDS_VR_BBG;
        } else {
            if (mst == 1) {
                region = NDS_VR_BBG;
                at = 0x8000;
            } else if (mst == 2) {
                region = NDS_VR_BOBJ;
            }
        }
        /* Extended palettes and texture slots are not CPU-visible. */
        if (region >= 0) map_pages(nds->vmap9[region], region_pages[region], at, mem, size);
    }
}

static NdsVramPage *vpage9(Nds *nds, uint32_t a) {
    unsigned region = (a >> 21) & 7;
    if (region > NDS_VR_LCDC) region = NDS_VR_LCDC;
    return &nds->vmap9[region][(a >> 14) & (region_pages[region] - 1)];
}

static NdsVramPage *vpage7(Nds *nds, uint32_t a) { return &nds->vmap7[(a >> 14) & 15]; }

static uint32_t vram_read(const NdsVramPage *p, uint32_t a, int size) {
    uint32_t v = 0;
    for (int i = 0; i < p->count; i++) v |= get_sized(p->bank[i] + (a & 0x3FFF), size);
    return v;
}

static void vram_write(NdsVramPage *p, uint32_t a, uint32_t v, int size) {
    for (int i = 0; i < p->count; i++) put_sized(p->bank[i] + (a & 0x3FFF), v, size);
}

/* ---- Shared WRAM ---- */

void nds_map_wram(Nds *nds) {
    switch (nds->wramcnt & 3) {
    case 0:
        nds->wram9_offset = 0, nds->wram9_size = 0x8000;
        nds->wram7_offset = 0, nds->wram7_size = 0;
        break;
    case 1:
        nds->wram9_offset = 0x4000, nds->wram9_size = 0x4000;
        nds->wram7_offset = 0, nds->wram7_size = 0x4000;
        break;
    case 2:
        nds->wram9_offset = 0, nds->wram9_size = 0x4000;
        nds->wram7_offset = 0x4000, nds->wram7_size = 0x4000;
        break;
    default:
        nds->wram9_offset = 0, nds->wram9_size = 0;
        nds->wram7_offset = 0, nds->wram7_size = 0x8000;
        break;
    }
}

void nds_mem_reset(Nds *nds) {
    memset(nds->main_ram, 0, sizeof nds->main_ram);
    memset(nds->shared_wram, 0, sizeof nds->shared_wram);
    memset(nds->arm7_wram, 0, sizeof nds->arm7_wram);
    memset(nds->vram, 0, sizeof nds->vram);
    memset(nds->palette, 0, sizeof nds->palette);
    memset(nds->oam, 0, sizeof nds->oam);
    memset(nds->itcm, 0, sizeof nds->itcm);
    memset(nds->dtcm, 0, sizeof nds->dtcm);
    memset(nds->vramcnt, 0, sizeof nds->vramcnt);
    nds->wramcnt = 0;
    nds_map_wram(nds);
    nds_map_vram(nds);
}

/* ---- The empty GBA slot: ROM reads return the address bus, SRAM 0xFF. Only the
 * CPU that EXMEMCNT bit 7 gives the slot to sees it. ---- */

static uint32_t gba_slot(Nds *nds, int cpu, uint32_t a, int size) {
    int owner = (nds->io9[0x204] >> 7) & 1; /* 0 = ARM9 */
    if (owner != cpu) return 0;
    if ((a >> 24) >= 0x0A) return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
    {
        uint32_t lo = (a >> 1) & 0xFFFF, hi = ((a + 2) >> 1) & 0xFFFF;
        if (size == 4) return lo | (hi << 16);
        if (size == 2) return lo;
        return (a & 1) ? lo >> 8 : lo & 0xFF;
    }
}

/* ---- ARM9 ---- */

/* `kind`: 0 data read, 1 fetch, 2 write, 3 byte write. */
static inline uint8_t *ptr9(Nds *nds, uint32_t a, int kind) {
    const NdsCp15 *cp = &nds->cp15;
    if ((kind == 2 || kind == 3 ? cp->itcm_write : cp->itcm_read) && !(a & cp->itcm_mask))
        return nds->itcm + (a & (NDS_ITCM_SIZE - 1));
    if (kind != 1 && (kind >= 2 ? cp->dtcm_write : cp->dtcm_read) &&
        (a & cp->dtcm_mask) == cp->dtcm_base)
        return nds->dtcm + (a & (NDS_DTCM_SIZE - 1));
    switch (a >> 24) {
    case 0x02:
        return nds->main_ram + (a & (NDS_MAIN_RAM_SIZE - 1));
    case 0x03:
        return nds->wram9_size ? nds->shared_wram + nds->wram9_offset + (a & (nds->wram9_size - 1))
                               : NULL;
    case 0x05:
        return kind == 3 ? NULL : nds->palette + (a & 0x7FF);
    case 0x06: {
        NdsVramPage *p;
        if (kind == 3) return NULL;
        p = vpage9(nds, a);
        return p->count == 1 ? p->bank[0] + (a & 0x3FFF) : NULL;
    }
    case 0x07:
        return kind == 3 ? NULL : nds->oam + (a & 0x7FF);
    case 0xFF:
        return kind < 2 && a >= 0xFFFF0000u ? nds->bios9 + (a & (NDS_BIOS9_SIZE - 1)) : NULL;
    default:
        return NULL;
    }
}

static uint32_t slow9_read(Nds *nds, uint32_t a, int size) {
    switch (a >> 24) {
    case 0x04: {
        uint32_t v = nds_io_read(nds, 0, a & ~3u) >> ((a & 3) * 8);
        return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
    }
    case 0x06:
        return vram_read(vpage9(nds, a), a, size);
    case 0x08: case 0x09: case 0x0A:
        return gba_slot(nds, 0, a, size);
    default:
        return 0;
    }
}

static void slow9_write(Nds *nds, uint32_t a, uint32_t v, int size) {
    switch (a >> 24) {
    case 0x04: {
        unsigned shift = (a & 3) * 8;
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        nds_io_write(nds, 0, a & ~3u, v << shift, mask << shift);
        break;
    }
    case 0x06:
        if (size > 1) vram_write(vpage9(nds, a), a, v, size); /* byte writes are ignored */
        break;
    default:
        break;
    }
}

uint32_t nds9_read32(Nds *nds, uint32_t a) {
    uint8_t *p;
    a &= ~3u;
    p = ptr9(nds, a, 0);
    return p ? get32(p) : slow9_read(nds, a, 4);
}
uint16_t nds9_read16(Nds *nds, uint32_t a) {
    uint8_t *p;
    a &= ~1u;
    p = ptr9(nds, a, 0);
    return p ? get16(p) : (uint16_t)slow9_read(nds, a, 2);
}
uint8_t nds9_read8(Nds *nds, uint32_t a) {
    uint8_t *p = ptr9(nds, a, 0);
    return p ? *p : (uint8_t)slow9_read(nds, a, 1);
}
uint32_t nds9_fetch32(Nds *nds, uint32_t a) {
    uint8_t *p;
    a &= ~3u;
    p = ptr9(nds, a, 1);
    return p ? get32(p) : slow9_read(nds, a, 4);
}
uint16_t nds9_fetch16(Nds *nds, uint32_t a) {
    uint8_t *p;
    a &= ~1u;
    p = ptr9(nds, a, 1);
    return p ? get16(p) : (uint16_t)slow9_read(nds, a, 2);
}
void nds9_write32(Nds *nds, uint32_t a, uint32_t v) {
    uint8_t *p;
    a &= ~3u;
    p = ptr9(nds, a, 2);
    if (p) put32(p, v);
    else slow9_write(nds, a, v, 4);
}
void nds9_write16(Nds *nds, uint32_t a, uint16_t v) {
    uint8_t *p;
    a &= ~1u;
    p = ptr9(nds, a, 2);
    if (p) put16(p, v);
    else slow9_write(nds, a, v, 2);
}
void nds9_write8(Nds *nds, uint32_t a, uint8_t v) {
    uint8_t *p = ptr9(nds, a, 3);
    if (p) *p = v;
    else slow9_write(nds, a, v, 1);
}

/* ---- ARM7 ---- */

static inline uint8_t *ptr7(Nds *nds, uint32_t a, int write) {
    switch (a >> 24) {
    case 0x00:
        return !write && a < NDS_BIOS7_SIZE ? nds->bios7 + a : NULL;
    case 0x02:
        return nds->main_ram + (a & (NDS_MAIN_RAM_SIZE - 1));
    case 0x03:
        /* Shared WRAM below 0x03800000, or the ARM7's own when none is allocated. */
        if (a < 0x03800000u && nds->wram7_size)
            return nds->shared_wram + nds->wram7_offset + (a & (nds->wram7_size - 1));
        return nds->arm7_wram + (a & (NDS_ARM7_WRAM_SIZE - 1));
    case 0x06: {
        NdsVramPage *p = vpage7(nds, a);
        return p->count == 1 ? p->bank[0] + (a & 0x3FFF) : NULL;
    }
    default:
        return NULL;
    }
}

static uint32_t slow7_read(Nds *nds, uint32_t a, int size) {
    switch (a >> 24) {
    case 0x04: {
        uint32_t v;
        if (a >= 0x04800000u) return 0; /* wifi: not emulated */
        v = nds_io_read(nds, 1, a & ~3u) >> ((a & 3) * 8);
        return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
    }
    case 0x06:
        return vram_read(vpage7(nds, a), a, size);
    case 0x08: case 0x09: case 0x0A:
        return gba_slot(nds, 1, a, size);
    default:
        return 0;
    }
}

static void slow7_write(Nds *nds, uint32_t a, uint32_t v, int size) {
    switch (a >> 24) {
    case 0x04: {
        unsigned shift = (a & 3) * 8;
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        if (a >= 0x04800000u) return;
        nds_io_write(nds, 1, a & ~3u, v << shift, mask << shift);
        break;
    }
    case 0x06:
        vram_write(vpage7(nds, a), a, v, size);
        break;
    default:
        break;
    }
}

uint32_t nds7_read32(Nds *nds, uint32_t a) {
    uint8_t *p;
    a &= ~3u;
    p = ptr7(nds, a, 0);
    return p ? get32(p) : slow7_read(nds, a, 4);
}
uint16_t nds7_read16(Nds *nds, uint32_t a) {
    uint8_t *p;
    a &= ~1u;
    p = ptr7(nds, a, 0);
    return p ? get16(p) : (uint16_t)slow7_read(nds, a, 2);
}
uint8_t nds7_read8(Nds *nds, uint32_t a) {
    uint8_t *p = ptr7(nds, a, 0);
    return p ? *p : (uint8_t)slow7_read(nds, a, 1);
}
void nds7_write32(Nds *nds, uint32_t a, uint32_t v) {
    uint8_t *p;
    a &= ~3u;
    p = ptr7(nds, a, 1);
    if (p) put32(p, v);
    else slow7_write(nds, a, v, 4);
}
void nds7_write16(Nds *nds, uint32_t a, uint16_t v) {
    uint8_t *p;
    a &= ~1u;
    p = ptr7(nds, a, 1);
    if (p) put16(p, v);
    else slow7_write(nds, a, v, 2);
}
void nds7_write8(Nds *nds, uint32_t a, uint8_t v) {
    uint8_t *p = ptr7(nds, a, 1);
    if (p) *p = v;
    else slow7_write(nds, a, v, 1);
}
