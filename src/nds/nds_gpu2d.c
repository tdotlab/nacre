/* The two 2D engines, as far as M1 needs them (GBATEK, "DS Video"): display
 * mode 0 (white), 1 (backdrop only until layers arrive in M2), 2 (engine A
 * shows a VRAM bank directly, "VRAM display" / LCDC mode) and 3 (main memory
 * display, shown black until the display FIFO arrives). */

#include "nds_internal.h"

static uint32_t rgb555(uint32_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* Bank offsets within nds->vram, A to D (each 128 KiB). */
static const uint32_t bank_offset[4] = {0x00000, 0x20000, 0x40000, 0x60000};

static void render_engine(Nds *nds, int engine, uint32_t line, uint32_t *out) {
    const uint8_t *io = nds->io9 + (engine ? 0x1000 : 0);
    uint32_t dispcnt = (uint32_t)io[0] | ((uint32_t)io[1] << 8) | ((uint32_t)io[2] << 16) |
                       ((uint32_t)io[3] << 24);
    uint32_t mode = (dispcnt >> 16) & 3, fill;
    if (engine) mode &= 1; /* engine B has only modes 0 and 1 */
    switch (mode) {
    case 0:
        fill = 0xFFFFFF;
        break;
    case 1:
        fill = rgb555(rd16(nds->palette + (engine ? 0x400 : 0)));
        break;
    case 2: {
        const uint8_t *src = nds->vram + bank_offset[(dispcnt >> 18) & 3] + line * 512;
        for (int x = 0; x < NDS_SCREEN_WIDTH; x++) out[x] = rgb555(rd16(src + x * 2));
        return;
    }
    default:
        fill = 0;
        break;
    }
    for (int x = 0; x < NDS_SCREEN_WIDTH; x++) out[x] = fill;
}

void nds_render_line(Nds *nds, uint32_t line) {
    uint32_t *top = nds->framebuffer + line * NDS_FRAME_WIDTH;
    uint32_t *bottom = top + NDS_SCREEN_HEIGHT * NDS_FRAME_WIDTH;
    if (line >= NDS_SCREEN_HEIGHT) return;
    if (!(nds->powcnt1 & 1)) {
        for (int x = 0; x < NDS_SCREEN_WIDTH; x++) top[x] = bottom[x] = 0;
        return;
    }
    /* POWCNT1 bit 15 puts engine A on the top screen. */
    render_engine(nds, 0, line, (nds->powcnt1 & 0x8000) ? top : bottom);
    render_engine(nds, 1, line, (nds->powcnt1 & 0x8000) ? bottom : top);
}
