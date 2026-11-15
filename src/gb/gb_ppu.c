/* Picture processing unit. Each visible line is rendered when the PPU enters
 * mode 3, using the registers in effect at that moment; mode lengths and STAT
 * interrupts follow DMG line timing. */

#include "gb_internal.h"

#include <string.h>

void gb_ppu_reset(Gb *g) {
    GbPpu *p = &g->ppu;
    memset(p, 0, sizeof *p);
    p->lcdc = 0x91;
    p->bgp = 0xFC;
    p->obp0 = 0xFF;
    p->obp1 = 0xFF;
    p->dma = 0xFF;
    p->mode = 2;
    p->mode3_end = 80 + 172;
    p->window_triggered = p->wy == 0;
}

static void update_stat(Gb *g) {
    GbPpu *p = &g->ppu;
    int line = 0;
    if (p->lcdc & 0x80) {
        line = ((p->stat & 0x40) && p->ly == p->lyc) ||
               ((p->stat & 0x08) && p->mode == 0) ||
               ((p->stat & 0x10) && p->mode == 1) ||
               ((p->stat & 0x20) && p->mode == 2);
    }
    if (line && !p->stat_line) g->if_reg |= GB_IRQ_STAT;
    p->stat_line = line;
}

static uint8_t tile_pixel(const Gb *g, uint16_t row_address, int bit) {
    uint8_t lo = g->vram[row_address - 0x8000];
    uint8_t hi = g->vram[row_address + 1 - 0x8000];
    return (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
}

static uint16_t bg_tile_row(const Gb *g, uint16_t map_base, int tx, int ty, int row) {
    const GbPpu *p = &g->ppu;
    uint8_t tile = g->vram[map_base + ty * 32 + tx - 0x8000];
    if (p->lcdc & 0x10) return (uint16_t)(0x8000 + tile * 16 + row * 2);
    return (uint16_t)(0x9000 + (int8_t)tile * 16 + row * 2);
}

static void render_line(Gb *g) {
    GbPpu *p = &g->ppu;
    uint8_t color_index[GB_SCREEN_WIDTH];
    uint8_t *out = &p->back[p->ly * GB_SCREEN_WIDTH];
    int sprites[10];
    int count = 0;
    int x;
    int window_used = 0;

    /* Background and window */
    for (x = 0; x < GB_SCREEN_WIDTH; x++) {
        uint8_t ci = 0;
        if (p->lcdc & 0x01) {
            int wx = (int)p->wx - 7;
            if ((p->lcdc & 0x20) && p->window_triggered && p->wx <= 166 && x >= wx) {
                uint16_t map = (p->lcdc & 0x40) ? 0x9C00 : 0x9800;
                int px = x - wx;
                int py = p->window_line;
                ci = tile_pixel(g, bg_tile_row(g, map, px >> 3, (py >> 3) & 31, py & 7), 7 - (px & 7));
                window_used = 1;
            } else {
                uint16_t map = (p->lcdc & 0x08) ? 0x9C00 : 0x9800;
                int px = (x + p->scx) & 255;
                int py = (p->ly + p->scy) & 255;
                ci = tile_pixel(g, bg_tile_row(g, map, px >> 3, py >> 3, py & 7), 7 - (px & 7));
            }
            out[x] = (uint8_t)((p->bgp >> (ci * 2)) & 3);
        } else {
            out[x] = 0; /* background off shows white on DMG */
        }
        color_index[x] = ci;
    }
    if (window_used) p->window_line++;

    /* Sprites: the first ten in OAM order that cover this line. */
    if (p->lcdc & 0x02) {
        int height = (p->lcdc & 0x04) ? 16 : 8;
        int i;
        for (i = 0; i < 40 && count < 10; i++) {
            int y = (int)g->oam[i * 4] - 16;
            if (p->ly >= y && p->ly < y + height) sprites[count++] = i;
        }
        /* DMG priority: lower X first, then lower OAM index (insertion sort keeps order stable). */
        for (i = 1; i < count; i++) {
            int s = sprites[i];
            int j = i - 1;
            while (j >= 0 && g->oam[sprites[j] * 4 + 1] > g->oam[s * 4 + 1]) {
                sprites[j + 1] = sprites[j];
                j--;
            }
            sprites[j + 1] = s;
        }
        for (x = 0; x < GB_SCREEN_WIDTH; x++) {
            for (i = 0; i < count; i++) {
                const uint8_t *o = &g->oam[sprites[i] * 4];
                int sx = (int)o[1] - 8;
                int row;
                int col;
                uint8_t tile;
                uint8_t ci;
                if (x < sx || x >= sx + 8) continue;
                row = p->ly - ((int)o[0] - 16);
                if (o[3] & 0x40) row = height - 1 - row;
                tile = o[2];
                if (height == 16) tile &= 0xFE;
                col = x - sx;
                if (o[3] & 0x20) col = 7 - col;
                ci = tile_pixel(g, (uint16_t)(0x8000 + tile * 16 + row * 2), 7 - col);
                if (ci == 0) continue; /* transparent: a lower-priority sprite may show */
                if (!((o[3] & 0x80) && color_index[x] != 0)) {
                    uint8_t palette = (o[3] & 0x10) ? p->obp1 : p->obp0;
                    out[x] = (uint8_t)((palette >> (ci * 2)) & 3);
                }
                break;
            }
        }
    }

    p->mode3_end = 80 + 172 + (p->scx & 7) + count * 6;
}

void gb_ppu_step(Gb *g, int dots) {
    GbPpu *p = &g->ppu;
    if (!(p->lcdc & 0x80)) {
        p->off_cycles += (uint32_t)dots;
        if (p->off_cycles >= GB_CYCLES_PER_FRAME) {
            p->off_cycles -= GB_CYCLES_PER_FRAME;
            memset(p->front, 0, sizeof p->front);
            p->frame_count++;
        }
        return;
    }
    while (dots-- > 0) {
        p->dot++;
        if (p->ly < 144) {
            if (p->dot == 80) {
                render_line(g);
                p->mode = 3;
                update_stat(g);
            } else if (p->dot == p->mode3_end) {
                p->mode = 0;
                update_stat(g);
            }
        }
        if (p->dot >= 456) {
            p->dot = 0;
            p->ly++;
            if (p->ly == 144) {
                p->mode = 1;
                g->if_reg |= GB_IRQ_VBLANK;
                memcpy(p->front, p->back, sizeof p->front);
                p->frame_count++;
            } else if (p->ly > 153) {
                p->ly = 0;
                p->window_line = 0;
                p->window_triggered = 0;
            }
            if (p->ly < 144) {
                p->mode = 2;
                if (p->ly == p->wy) p->window_triggered = 1;
            }
            update_stat(g);
        }
    }
}

uint8_t gb_ppu_read(Gb *g, uint16_t a) {
    GbPpu *p = &g->ppu;
    switch (a) {
    case 0xFF40: return p->lcdc;
    case 0xFF41:
        return (uint8_t)(0x80 | (p->stat & 0x78) | (p->ly == p->lyc ? 0x04 : 0) |
                         ((p->lcdc & 0x80) ? p->mode : 0));
    case 0xFF42: return p->scy;
    case 0xFF43: return p->scx;
    case 0xFF44: return p->ly;
    case 0xFF45: return p->lyc;
    case 0xFF46: return p->dma;
    case 0xFF47: return p->bgp;
    case 0xFF48: return p->obp0;
    case 0xFF49: return p->obp1;
    case 0xFF4A: return p->wy;
    case 0xFF4B: return p->wx;
    default: return 0xFF;
    }
}

void gb_ppu_write(Gb *g, uint16_t a, uint8_t v) {
    GbPpu *p = &g->ppu;
    switch (a) {
    case 0xFF40: {
        int was_on = (p->lcdc & 0x80) != 0;
        p->lcdc = v;
        if (was_on && !(v & 0x80)) {
            p->ly = 0;
            p->dot = 0;
            p->mode = 0;
            p->off_cycles = 0;
        } else if (!was_on && (v & 0x80)) {
            p->ly = 0;
            p->dot = 0;
            p->mode = 0; /* the first line after enabling skips the OAM scan */
            p->mode3_end = 80 + 172;
            p->window_line = 0;
            p->window_triggered = p->wy == 0;
        }
        update_stat(g);
        break;
    }
    case 0xFF41: p->stat = (uint8_t)(v & 0x78); update_stat(g); break;
    case 0xFF42: p->scy = v; break;
    case 0xFF43: p->scx = v; break;
    case 0xFF44: break; /* read-only */
    case 0xFF45: p->lyc = v; update_stat(g); break;
    case 0xFF46: p->dma = v; break;
    case 0xFF47: p->bgp = v; break;
    case 0xFF48: p->obp0 = v; break;
    case 0xFF49: p->obp1 = v; break;
    case 0xFF4A: p->wy = v; break;
    case 0xFF4B: p->wx = v; break;
    default: break;
    }
}
