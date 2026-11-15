/* Picture processing unit.
 *
 * Timing follows the DMG as measured by the Mooneye PPU tests, in this model's
 * dot count (the CPU sees the PPU after each 4-dot step):
 *
 *   dot 0      mode 2 (OAM scan) starts; LY=LYC is compared
 *   dot 76     VRAM reads lock and OAM writes pass for one M-cycle
 *   dot 80     mode 3; OAM writes and VRAM writes lock
 *   mode3_end  mode 0; 252 + SCX%8 + sprite and window penalties
 *   dot 452    LY increments and the LY=LYC flag drops until the next compare;
 *              OAM reads lock for the next visible line
 *
 * The first line after the LCD is switched on skips the OAM scan: it reads as
 * mode 0 with OAM open until dot 80. On line 153 LY reads 0 after four dots.
 *
 * Pixels are drawn in spans. A write to a PPU register during mode 3 first draws
 * the line up to the current pixel, so mid-line palette, scroll, and LCDC
 * changes land at about the right place. The pixel clock assumes all of the
 * line's penalty dots come before its first pixel; a full pixel FIFO is not
 * modelled. */

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
    p->lyc_flag = p->ly == p->lyc;
    p->window_triggered = p->wy == 0;
}

static int lcd_on(const GbPpu *p) { return (p->lcdc & 0x80) != 0; }

static int visible(const GbPpu *p) { return p->line < 144; }

/* The STAT interrupt is the OR of its enabled sources; it fires on a rising edge. */
static int stat_sources(const GbPpu *p, uint8_t enables) {
    return ((enables & 0x40) && p->lyc_flag) || ((enables & 0x08) && p->mode == 0) ||
           ((enables & 0x10) && p->mode == 1) ||
           ((enables & 0x20) && (p->mode == 2 || (p->line == 144 && p->dot == 0)));
}

static void update_stat(Gb *g) {
    GbPpu *p = &g->ppu;
    int line;
    if (!lcd_on(p)) return; /* frozen while the LCD is off */
    line = stat_sources(p, p->stat);
    if (line && !p->stat_line) g->if_reg |= GB_IRQ_STAT;
    p->stat_line = line;
}

static void compare_lyc(Gb *g) {
    g->ppu.lyc_flag = g->ppu.ly == g->ppu.lyc;
    update_stat(g);
}

/* ---- access windows -------------------------------------------------------------- */

int gb_ppu_vram_open(const Gb *g, int write) {
    const GbPpu *p = &g->ppu;
    int start;
    if (!lcd_on(p) || !visible(p)) return 1;
    start = (write || p->first_line) ? 80 : 76;
    return p->dot < start || p->dot >= p->mode3_end;
}

int gb_ppu_oam_open(const Gb *g, int write) {
    const GbPpu *p = &g->ppu;
    if (!lcd_on(p)) return 1;
    if (!write && p->dot >= 452 && (p->line < 143 || p->line == 153)) return 0;
    if (!visible(p)) return 1;
    if (p->first_line && p->dot < 80) return 1;
    if (write && p->dot >= 76 && p->dot < 80) return 1;
    return p->dot >= p->mode3_end;
}

/* ---- OAM corruption bug ---------------------------------------------------------------
 * During the mode 2 scan the PPU reads one 8-byte OAM row per M-cycle. A CPU access
 * to $FE00-$FEFF in that M-cycle, or a 16-bit increment or decrement of a register
 * holding such an address, garbles the row the PPU is on (Pan Docs, "OAM Corruption
 * Bug"). Every formula is bitwise, so it can work a byte at a time. A CPU access at
 * dot 4k hits row k+1, which gives the 19-M-cycle window Blargg's oam_bug tests
 * measure; row 0 is never hit. */

#ifndef GB_OAM_ROW_OFFSET
#define GB_OAM_ROW_OFFSET 1 /* the first scan M-cycle touches row 1, so row 0 is safe */
#endif

static int oam_scan_row(const GbPpu *p) {
    int row;
    if (!lcd_on(p) || !visible(p) || p->first_line || p->dot >= 80) return -1;
    row = p->dot / 4 + GB_OAM_ROW_OFFSET;
    return row >= 1 && row < 20 ? row : -1;
}

static void corrupt_row(uint8_t *oam, int row, int read) {
    uint8_t *cur = oam + row * 8;
    const uint8_t *prev = cur - 8;
    int j;
    for (j = 0; j < 2; j++) {
        uint8_t a = cur[j], b = prev[j], c = prev[4 + j];
        cur[j] = read ? (uint8_t)(b | (a & c)) : (uint8_t)(((a ^ c) & (b ^ c)) ^ c);
    }
    for (j = 2; j < 8; j++) cur[j] = prev[j];
}

void gb_oam_bug(Gb *g, GbOamBug kind) {
    int row = oam_scan_row(&g->ppu);
    if (row < 0) return;
    if (kind == GB_OAM_BUG_READ_IDU && row >= 4 && row < 19) {
        uint8_t *cur = g->oam + row * 8;
        uint8_t *prev = cur - 8;
        uint8_t *prev2 = cur - 16;
        int j;
        for (j = 0; j < 2; j++) {
            uint8_t a = prev2[j], b = prev[j], c = cur[j], d = prev[4 + j];
            prev[j] = (uint8_t)((b & (a | c | d)) | (a & c & d));
        }
        for (j = 0; j < 8; j++) cur[j] = prev2[j] = prev[j];
    }
    corrupt_row(g->oam, row, kind != GB_OAM_BUG_WRITE);
}

static int stat_mode(const GbPpu *p) {
    if (!lcd_on(p)) return 0;
    if (!visible(p)) return 1;
    if (p->first_line && p->dot < 80) return 0;
    if (p->dot < 80) return 2;
    return p->dot < p->mode3_end ? 3 : 0;
}

/* ---- drawing ------------------------------------------------------------------------ */

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

static int window_covers(const GbPpu *p, int x) {
    return (p->lcdc & 0x21) == 0x21 && p->window_triggered && p->wx <= 166 && x >= (int)p->wx - 7;
}

/* Draw pixels [render_x, end) of the current line with the registers as they are now. */
static void render_until(Gb *g, int end) {
    GbPpu *p = &g->ppu;
    uint8_t *out = &p->back[p->line * GB_SCREEN_WIDTH];
    int x;
    if (end > GB_SCREEN_WIDTH) end = GB_SCREEN_WIDTH;
    for (x = p->render_x; x < end; x++) {
        uint8_t ci = 0;
        uint8_t shade = 0; /* background off shows white on DMG */
        int i;
        if (p->lcdc & 0x01) {
            if (window_covers(p, x)) {
                uint16_t map = (p->lcdc & 0x40) ? 0x9C00 : 0x9800;
                int px = x - ((int)p->wx - 7);
                int py = p->window_line;
                ci = tile_pixel(g, bg_tile_row(g, map, px >> 3, (py >> 3) & 31, py & 7), 7 - (px & 7));
                p->window_drawn = 1;
            } else {
                uint16_t map = (p->lcdc & 0x08) ? 0x9C00 : 0x9800;
                int px = (x + p->scx) & 255;
                int py = (p->line + p->scy) & 255;
                ci = tile_pixel(g, bg_tile_row(g, map, px >> 3, py >> 3, py & 7), 7 - (px & 7));
            }
            shade = (uint8_t)((p->bgp >> (ci * 2)) & 3);
        }
        p->bg_index[x] = ci;
        if (p->lcdc & 0x02) {
            int height = (p->lcdc & 0x04) ? 16 : 8;
            for (i = 0; i < p->sprite_count; i++) {
                const uint8_t *o = &g->oam[p->sprites[i] * 4];
                int sx = (int)o[1] - 8;
                int row = p->line - ((int)o[0] - 16);
                int col;
                uint8_t tile;
                uint8_t sci;
                if (x < sx || x >= sx + 8 || row < 0 || row >= height) continue;
                if (o[3] & 0x40) row = height - 1 - row;
                tile = o[2];
                if (height == 16) tile &= 0xFE;
                col = x - sx;
                if (o[3] & 0x20) col = 7 - col;
                sci = tile_pixel(g, (uint16_t)(0x8000 + tile * 16 + row * 2), 7 - col);
                if (sci == 0) continue; /* transparent: a lower-priority sprite may show */
                if (!((o[3] & 0x80) && ci != 0)) {
                    uint8_t palette = (o[3] & 0x10) ? p->obp1 : p->obp0;
                    shade = (uint8_t)((palette >> (sci * 2)) & 3);
                }
                break;
            }
        }
        out[x] = shade;
    }
    if (end > p->render_x) p->render_x = end;
}

/* The pixel the LCD has reached at the current dot. */
static int current_pixel(const GbPpu *p) {
    int x = p->dot - (p->mode3_end - GB_SCREEN_WIDTH);
    return x < 0 ? 0 : x;
}

/* Mode 3 begins: pick the line's sprites and work out how long the line takes. */
static void begin_mode3(Gb *g) {
    GbPpu *p = &g->ppu;
    int height = (p->lcdc & 0x04) ? 16 : 8;
    int penalty = 0;
    uint8_t seen[32];
    int i;

    /* The OAM scan takes the first ten sprites in OAM order that cover this line,
     * whether or not sprites are enabled. */
    p->sprite_count = 0;
    for (i = 0; i < 40 && p->sprite_count < 10; i++) {
        int y = (int)g->oam[i * 4] - 16;
        if (p->line >= y && p->line < y + height) p->sprites[p->sprite_count++] = (uint8_t)i;
    }
    /* DMG drawing priority: lower X first, then lower OAM index (stable insertion sort). */
    for (i = 1; i < p->sprite_count; i++) {
        uint8_t s = p->sprites[i];
        int j = i - 1;
        while (j >= 0 && g->oam[p->sprites[j] * 4 + 1] > g->oam[s * 4 + 1]) {
            p->sprites[j + 1] = p->sprites[j];
            j--;
        }
        p->sprites[j + 1] = s;
    }

    /* Each fetched sprite costs 6 dots, and the first in each background tile
     * column also waits for that tile's fetch to finish (Pan Docs). */
    if (p->lcdc & 0x02) {
        memset(seen, 0, sizeof seen);
        for (i = 0; i < p->sprite_count; i++) {
            int x = g->oam[p->sprites[i] * 4 + 1];
            int column;
            if (x >= 168) continue; /* never reached by the fetcher */
            column = (x + (p->scx & 7)) >> 3;
            if (!seen[column]) {
                int offset = x == 0 ? 0 : (x + p->scx) & 7;
                seen[column] = 1;
                if (offset < 5) penalty += 5 - offset;
            }
            penalty += 6;
        }
    }
    /* Mooneye's sprite tables put the end three dots earlier than a plain sum;
     * the line without sprites keeps its measured 252 + SCX%8. */
    if (penalty) penalty -= 3;
    if ((p->lcdc & 0x01) && window_covers(p, GB_SCREEN_WIDTH - 1)) penalty += 6;

    p->mode3_end = 80 + 172 + (p->scx & 7) + penalty;
    p->render_x = 0;
    p->window_drawn = 0;
}

static void end_mode3(Gb *g) {
    GbPpu *p = &g->ppu;
    render_until(g, GB_SCREEN_WIDTH);
    if (p->window_drawn) p->window_line++;
}

/* ---- stepping ------------------------------------------------------------------------- */

static void start_line(Gb *g) {
    GbPpu *p = &g->ppu;
    p->dot = 0;
    p->first_line = 0;
    p->line++;
    if (p->line > 153) {
        p->line = 0;
        p->window_line = 0;
        p->window_triggered = 0;
    }
    if (p->line == 144) {
        p->mode = 1;
        g->if_reg |= GB_IRQ_VBLANK;
        memcpy(p->front, p->back, sizeof p->front);
        p->frame_count++;
    } else if (p->line < 144) {
        p->mode = 2;
        if (p->line == p->wy) p->window_triggered = 1;
    }
    p->ly = (uint8_t)p->line;
    compare_lyc(g);
}

void gb_ppu_step(Gb *g, int dots) {
    GbPpu *p = &g->ppu;
    if (!lcd_on(p)) {
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
        if (p->dot >= 456) {
            start_line(g);
            continue;
        }
        if (visible(p)) {
            if (p->dot == 80) {
                begin_mode3(g);
                p->mode = 3;
                update_stat(g);
            } else if (p->dot == p->mode3_end) {
                end_mode3(g);
                p->mode = 0;
                update_stat(g);
            }
        }
        if (p->dot == 452 && p->line < 153) {
            p->ly = (uint8_t)(p->line + 1);
            p->lyc_flag = 0;
            update_stat(g);
        } else if (p->line == 153 && p->dot == 4) {
            p->ly = 0;
            p->lyc_flag = 0;
            update_stat(g);
        } else if (p->line == 153 && p->dot == 8) {
            compare_lyc(g);
        }
    }
}

/* ---- registers ------------------------------------------------------------------------- */

uint8_t gb_ppu_read(Gb *g, uint16_t a) {
    GbPpu *p = &g->ppu;
    switch (a) {
    case 0xFF40: return p->lcdc;
    case 0xFF41: return (uint8_t)(0x80 | (p->stat & 0x78) | (p->lyc_flag ? 0x04 : 0) | stat_mode(p));
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

/* LY=LYC compares stop on the dots where LY has just changed. */
static int lyc_compare_paused(const GbPpu *p) {
    return (p->dot >= 452 && p->line < 153) || (p->line == 153 && p->dot >= 4 && p->dot < 8);
}

void gb_ppu_write(Gb *g, uint16_t a, uint8_t v) {
    GbPpu *p = &g->ppu;
    if (lcd_on(p) && visible(p) && p->mode == 3 && a != 0xFF41 && a != 0xFF45 && a != 0xFF46) {
        render_until(g, current_pixel(p)); /* earlier pixels keep the old value */
    }
    switch (a) {
    case 0xFF40: {
        int was_on = lcd_on(p);
        p->lcdc = v;
        if (was_on && !(v & 0x80)) {
            /* LY resets; the LY=LYC flag and the interrupt line hold their values. */
            p->ly = 0;
            p->line = 0;
            p->dot = 0;
            p->mode = 0;
            p->off_cycles = 0;
        } else if (!was_on && (v & 0x80)) {
            p->ly = 0;
            p->line = 0;
            p->dot = 0;
            p->mode = 0; /* the first line skips the OAM scan */
            p->first_line = 1;
            p->mode3_end = 80 + 172;
            p->window_line = 0;
            p->window_triggered = p->wy == 0;
            compare_lyc(g);
        }
        break;
    }
    case 0xFF41:
        /* DMG: the write briefly enables every source, so an active HBlank,
         * VBlank, or LY=LYC condition raises an interrupt. */
        if (lcd_on(p) && !p->stat_line && (p->lyc_flag || p->mode == 0 || p->mode == 1)) {
            g->if_reg |= GB_IRQ_STAT;
        }
        p->stat = (uint8_t)(v & 0x78);
        update_stat(g);
        break;
    case 0xFF42: p->scy = v; break;
    case 0xFF43: p->scx = v; break;
    case 0xFF44: break; /* read-only */
    case 0xFF45:
        p->lyc = v;
        if (lcd_on(p) && !lyc_compare_paused(p)) compare_lyc(g);
        break;
    case 0xFF46: p->dma = v; break;
    case 0xFF47: p->bgp = v; break;
    case 0xFF48: p->obp0 = v; break;
    case 0xFF49: p->obp1 = v; break;
    case 0xFF4A: p->wy = v; break;
    case 0xFF4B: p->wx = v; break;
    default: break;
    }
}
