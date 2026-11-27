/* The two 2D engines (GBATEK, "DS Video" and its GBA "LCD" chapters): text,
 * affine, extended (16-bit map, 256-color and direct-color bitmap) and large
 * bitmap BGs, tile and bitmap OBJs with affine transforms, windows (including
 * the OBJ window), blending and brightness effects, mosaic, extended palettes,
 * master brightness, display capture and the main-memory display FIFO.
 *
 * Each visible line is drawn at its HBlank: every layer into a line buffer of
 * 15-bit colors (bit 15 marks an opaque pixel), then composited by priority.
 * BG0 in 3D mode stays transparent until the 3D engine exists (M6).
 *
 * Choices GBATEK leaves open, to check against an oracle: a bitmap OBJ with
 * alpha 0 is hidden, and otherwise blends at (alpha+1)/16 over the layer below;
 * semi-transparent and bitmap OBJs blend even where a window disables color
 * effects; a disabled engine (POWCNT1 bits 1/9) still draws. */

#include "nds_internal.h"

#include <string.h>

enum { LAYER_OBJ = 4, LAYER_BD = 5, NO_PRIO = 4 };
enum { BG_NONE, BG_TEXT, BG_AFFINE, BG_EXT16, BG_BMP8, BG_DIRECT, BG_LARGE, BG_3D };
enum { OBJ_NORMAL, OBJ_SEMI, OBJ_BITMAP };

#define OPAQUE 0x8000u

static inline uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static inline int32_t asr(int32_t v, int n) { return v < 0 ? ~(~v >> n) : v >> n; }

/* Reads through the engine's VRAM view; overlapping banks read ORed. */
static inline uint32_t vram8(const NdsVramPage *map, uint32_t mask, uint32_t a) {
    const NdsVramPage *p = &map[(a >> 14) & mask];
    uint32_t o = a & 0x3FFF, v = 0;
    for (unsigned i = 0; i < p->count; i++) v |= p->bank[i][o];
    return v;
}

static inline uint32_t vram16(const NdsVramPage *map, uint32_t mask, uint32_t a) {
    const NdsVramPage *p = &map[(a >> 14) & mask];
    uint32_t o = a & 0x3FFE, v = 0;
    for (unsigned i = 0; i < p->count; i++) v |= rd16(p->bank[i] + o);
    return v;
}

static inline uint32_t extpal(const NdsVramPage *slot, uint32_t index) {
    uint32_t v = 0;
    for (unsigned i = 0; i < slot->count; i++) v |= rd16(slot->bank[i] + ((index * 2) & 0x1FFF));
    return v;
}

typedef struct {
    Nds *nds;
    int engine;
    uint32_t line;
    const uint8_t *io;
    uint32_t dispcnt;
    const NdsVramPage *bg, *obj;
    uint32_t bg_mask, obj_mask;
    const uint8_t *pal; /* the engine's standard palettes: BG at 0, OBJ at 0x200 */
    const uint8_t *oam;
    uint32_t mosaic;
} Engine;

static inline uint32_t reg16(const Engine *e, uint32_t off) { return rd16(e->io + off); }
static inline uint32_t bg_color(const Engine *e, uint32_t index) { return rd16(e->pal + index * 2); }

/* ---- BGs ---- */

static int bg_kind(const Engine *e, int bg) {
    uint32_t mode = e->dispcnt & 7, cnt = reg16(e, 8 + bg * 2);
    int kind = BG_NONE;
    if (bg == 0 && !e->engine && ((e->dispcnt & 8) || mode == 6)) return BG_3D;
    if (mode == 7 || (mode == 6 && e->engine)) return BG_NONE;
    switch (bg) {
    case 0:
    case 1: kind = mode == 6 ? BG_NONE : BG_TEXT; break;
    case 2: kind = mode == 6 ? BG_LARGE : mode == 2 || mode == 4 ? BG_AFFINE : mode == 5 ? -1 : BG_TEXT; break;
    default: kind = mode == 0 ? BG_TEXT : mode <= 2 ? BG_AFFINE : mode <= 5 ? -1 : BG_NONE; break;
    }
    if (kind == -1) kind = !(cnt & 0x80) ? BG_EXT16 : (cnt & 4) ? BG_DIRECT : BG_BMP8;
    return kind;
}

/* The extended palette slot a BG uses: BG0/BG1 can move to slots 2/3. */
static const NdsVramPage *bg_slot(const Engine *e, int bg, uint32_t cnt) {
    int slot = bg >= 2 ? bg : bg + ((cnt & 0x2000) ? 2 : 0);
    return &e->nds->bg_extpal[e->engine][slot];
}

static void text_bg(const Engine *e, int bg, uint32_t line, uint16_t *out) {
    uint32_t cnt = reg16(e, 8 + bg * 2);
    uint32_t hofs = reg16(e, 0x10 + bg * 4) & 0x1FF, vofs = reg16(e, 0x12 + bg * 4) & 0x1FF;
    uint32_t wide = cnt & 0x4000, tall = cnt & 0x8000;
    uint32_t screen = ((cnt >> 8) & 31) * 0x800, chr = ((cnt >> 2) & 15) * 0x4000;
    uint32_t y, row, map_addr = UINT32_MAX, entry = 0;
    int deep = (cnt >> 7) & 1;
    const NdsVramPage *ext = deep && (e->dispcnt & (1u << 30)) ? bg_slot(e, bg, cnt) : NULL;
    if (!e->engine) {
        screen += ((e->dispcnt >> 27) & 7) * 0x10000;
        chr += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    y = (line + vofs) & (tall ? 511 : 255);
    row = screen + (y >= 256 ? (wide ? 0x1000 : 0x800) : 0) + ((y & 255) >> 3) * 64;
    for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
        uint32_t tx = (x + hofs) & (wide ? 511 : 255);
        uint32_t m = row + (tx >= 256 ? 0x800 : 0) + ((tx & 255) >> 3) * 2;
        uint32_t px, py, tile, c;
        if (m != map_addr) {
            map_addr = m;
            entry = vram16(e->bg, e->bg_mask, m);
        }
        tile = entry & 0x3FF;
        px = (entry & 0x400) ? 7 - (tx & 7) : tx & 7;
        py = (entry & 0x800) ? 7 - (y & 7) : y & 7;
        if (deep) {
            c = vram8(e->bg, e->bg_mask, chr + tile * 64 + py * 8 + px);
            out[x] = !c ? 0 : (uint16_t)(OPAQUE | (ext ? extpal(ext, (entry >> 12) * 256 + c) : bg_color(e, c)));
        } else {
            c = vram8(e->bg, e->bg_mask, chr + tile * 32 + py * 4 + px / 2);
            c = (px & 1) ? c >> 4 : c & 15;
            out[x] = !c ? 0 : (uint16_t)(OPAQUE | bg_color(e, (entry >> 12) * 16 + c));
        }
    }
}

static void affine_bg(const Engine *e, int bg, int kind, uint32_t line, uint16_t *out) {
    static const uint16_t bmp_w[4] = {128, 256, 512, 512}, bmp_h[4] = {128, 256, 256, 512};
    uint32_t cnt = reg16(e, 8 + bg * 2), r = 0x20 + (uint32_t)(bg - 2) * 0x10, size = cnt >> 14;
    int32_t pa = (int16_t)reg16(e, r), pb = (int16_t)reg16(e, r + 2);
    int32_t pc = (int16_t)reg16(e, r + 4), pd = (int16_t)reg16(e, r + 6);
    int32_t rx = e->nds->engine2d[e->engine].ref_x[bg - 2], ry = e->nds->engine2d[e->engine].ref_y[bg - 2];
    uint32_t w, h, screen = ((cnt >> 8) & 31) * 0x800, chr = ((cnt >> 2) & 15) * 0x4000;
    uint32_t bitmap = ((cnt >> 8) & 31) * 0x4000;
    int wrap = (cnt >> 13) & 1;
    const NdsVramPage *ext = kind == BG_EXT16 && (e->dispcnt & (1u << 30)) ? bg_slot(e, bg, cnt) : NULL;
    if (!e->engine) {
        screen += ((e->dispcnt >> 27) & 7) * 0x10000;
        chr += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    if (cnt & 0x40) { /* vertical mosaic: the reference point of the block's first line */
        int32_t m = (int32_t)(line % (((e->mosaic >> 4) & 15) + 1));
        rx -= pb * m;
        ry -= pd * m;
    }
    switch (kind) {
    case BG_AFFINE:
    case BG_EXT16: w = h = 128u << size; break;
    case BG_LARGE:
        if (size >= 2) {
            memset(out, 0, NDS_SCREEN_WIDTH * sizeof *out);
            return;
        }
        w = size ? 1024 : 512;
        h = size ? 512 : 1024;
        break;
    default: w = bmp_w[size], h = bmp_h[size]; break;
    }
    for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++, rx += pa, ry += pc) {
        uint32_t px = (uint32_t)asr(rx, 8), py = (uint32_t)asr(ry, 8), c;
        if (wrap) {
            px &= w - 1;
            py &= h - 1;
        } else if (px >= w || py >= h) {
            out[x] = 0;
            continue;
        }
        switch (kind) {
        case BG_AFFINE: {
            uint32_t tile = vram8(e->bg, e->bg_mask, screen + (py >> 3) * (w >> 3) + (px >> 3));
            c = vram8(e->bg, e->bg_mask, chr + tile * 64 + (py & 7) * 8 + (px & 7));
            out[x] = !c ? 0 : (uint16_t)(OPAQUE | bg_color(e, c));
            break;
        }
        case BG_EXT16: {
            uint32_t entry = vram16(e->bg, e->bg_mask, screen + ((py >> 3) * (w >> 3) + (px >> 3)) * 2);
            uint32_t tx = (entry & 0x400) ? 7 - (px & 7) : px & 7, ty = (entry & 0x800) ? 7 - (py & 7) : py & 7;
            c = vram8(e->bg, e->bg_mask, chr + (entry & 0x3FF) * 64 + ty * 8 + tx);
            out[x] = !c ? 0 : (uint16_t)(OPAQUE | (ext ? extpal(ext, (entry >> 12) * 256 + c) : bg_color(e, c)));
            break;
        }
        case BG_BMP8:
            c = vram8(e->bg, e->bg_mask, bitmap + py * w + px);
            out[x] = !c ? 0 : (uint16_t)(OPAQUE | bg_color(e, c));
            break;
        case BG_LARGE:
            c = vram8(e->bg, e->bg_mask, py * w + px);
            out[x] = !c ? 0 : (uint16_t)(OPAQUE | bg_color(e, c));
            break;
        default: /* direct color: bit 15 is the alpha flag */
            c = vram16(e->bg, e->bg_mask, bitmap + (py * w + px) * 2);
            out[x] = (c & 0x8000) ? (uint16_t)c : 0;
            break;
        }
    }
}

/* ---- OBJs ---- */

typedef struct {
    uint16_t color[NDS_SCREEN_WIDTH];
    uint8_t prio[NDS_SCREEN_WIDTH]; /* NO_PRIO where no OBJ pixel */
    uint8_t kind[NDS_SCREEN_WIDTH];
    uint8_t alpha[NDS_SCREEN_WIDTH];
    uint8_t window[NDS_SCREEN_WIDTH];
} ObjLine;

static const uint8_t obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

/* One texel of an OBJ, as an opaque-flagged color or 0. */
static uint32_t obj_texel(const Engine *e, uint32_t a0, uint32_t a2, uint32_t w, uint32_t tx, uint32_t ty) {
    uint32_t tile = a2 & 0x3FF, mode = (a0 >> 10) & 3, d = e->dispcnt, c;
    if (mode == 3) { /* bitmap OBJ */
        uint32_t base, stride;
        if (d & 0x40) {
            if (d & 0x20) return 0; /* reserved */
            base = tile * ((!e->engine && (d & 0x400000)) ? 256u : 128u);
            stride = w * 2;
        } else {
            uint32_t mask_x = (d & 0x20) ? 0x1F : 0x0F;
            base = (tile & mask_x) * 0x10 + (tile & ~mask_x) * 0x80;
            stride = (d & 0x20) ? 512 : 256;
        }
        c = vram16(e->obj, e->obj_mask, base + ty * stride + tx * 2);
        return (c & 0x8000) ? c : 0;
    }
    {
        int deep = (a0 >> 13) & 1;
        uint32_t tile_bytes = deep ? 64 : 32, addr;
        if (d & 0x10) {
            addr = tile * (32u << ((d >> 20) & 3)) + ((ty >> 3) * (w >> 3) + (tx >> 3)) * tile_bytes;
        } else {
            addr = ((deep ? tile & ~1u : tile) * 32 + (ty >> 3) * 1024 + (tx >> 3) * tile_bytes) & 0x7FFF;
        }
        if (deep) {
            c = vram8(e->obj, e->obj_mask, addr + (ty & 7) * 8 + (tx & 7));
            if (!c) return 0;
            if (d & 0x80000000u) return OPAQUE | extpal(&e->nds->obj_extpal[e->engine], (a2 >> 12) * 256 + c);
            return OPAQUE | rd16(e->pal + 0x200 + c * 2);
        }
        c = vram8(e->obj, e->obj_mask, addr + (ty & 7) * 4 + (tx & 7) / 2);
        c = (tx & 1) ? c >> 4 : c & 15;
        return c ? OPAQUE | rd16(e->pal + 0x200 + ((a2 >> 12) * 16 + c) * 2) : 0;
    }
}

static void render_objs(const Engine *e, ObjLine *o) {
    uint32_t mh = ((e->mosaic >> 8) & 15) + 1, mv = ((e->mosaic >> 12) & 15) + 1;
    memset(o->prio, NO_PRIO, sizeof o->prio);
    memset(o->window, 0, sizeof o->window);
    if (!(e->dispcnt & 0x1000)) return;
    for (int i = 0; i < 128; i++) {
        const uint8_t *attr = e->oam + i * 8;
        uint32_t a0 = rd16(attr), a1 = rd16(attr + 2), a2 = rd16(attr + 4);
        uint32_t affine = a0 & 0x100, mode = (a0 >> 10) & 3, shape = a0 >> 14, size = a1 >> 14;
        uint32_t w, h, bw, bh, row, prio = (a2 >> 10) & 3, alpha = a2 >> 12, mosaic = a0 & 0x1000;
        int32_t x0 = (int32_t)(a1 & 0x1FF), pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if ((!affine && (a0 & 0x200)) || shape == 3) continue;
        if (mode == 3 && alpha == 0) continue;
        w = bw = obj_w[shape][size];
        h = bh = obj_h[shape][size];
        if (affine && (a0 & 0x200)) bw *= 2, bh *= 2;
        row = (e->line - (a0 & 0xFF)) & 0xFF;
        if (row >= bh) continue;
        if (mosaic) {
            uint32_t m = ((e->line - e->line % mv) - (a0 & 0xFF)) & 0xFF;
            if (m < bh) row = m;
        }
        if (x0 >= 256) x0 -= 512;
        if (affine) {
            const uint8_t *p = e->oam + ((a1 >> 9) & 31) * 0x20;
            pa = (int16_t)rd16(p + 6);
            pb = (int16_t)rd16(p + 14);
            pc = (int16_t)rd16(p + 22);
            pd = (int16_t)rd16(p + 30);
        }
        for (uint32_t sx = 0; sx < bw; sx++) {
            int32_t x = x0 + (int32_t)sx, s = (int32_t)sx;
            uint32_t tx, ty, c;
            if (x < 0 || x >= NDS_SCREEN_WIDTH) continue;
            if (mode != 2 && o->prio[x] <= prio) continue; /* a lower OAM index wins ties */
            if (mosaic && mh > 1) {
                s -= x % (int32_t)mh;
                if (s < 0) s = 0;
            }
            if (affine) {
                int32_t dx = s - (int32_t)(bw / 2), dy = (int32_t)row - (int32_t)(bh / 2);
                tx = (uint32_t)(asr(pa * dx + pb * dy, 8) + (int32_t)(w / 2));
                ty = (uint32_t)(asr(pc * dx + pd * dy, 8) + (int32_t)(h / 2));
                if (tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - (uint32_t)s : (uint32_t)s;
                ty = (a1 & 0x2000) ? h - 1 - row : row;
            }
            c = obj_texel(e, a0, a2, w, tx, ty);
            if (!c) continue;
            if (mode == 2) {
                o->window[x] = 1;
                continue;
            }
            o->color[x] = (uint16_t)c;
            o->prio[x] = (uint8_t)prio;
            o->kind[x] = (uint8_t)(mode == 1 ? OBJ_SEMI : mode == 3 ? OBJ_BITMAP : OBJ_NORMAL);
            o->alpha[x] = (uint8_t)alpha;
        }
    }
}

/* ---- Windows and effects ---- */

static int in_window_x(uint32_t winh, uint32_t x) {
    uint32_t x1 = winh >> 8, x2 = winh & 0xFF;
    if (!x1 && !x2) return 0;
    if (!x2) x2 = 256; /* X2 = 0 is the right edge */
    return x1 <= x2 ? x >= x1 && x < x2 : x >= x1 || x < x2;
}

/* Per pixel, the enabled layers (bits 0-4) and effects (bit 5). */
static void window_line(const Engine *e, const ObjLine *o, uint8_t *ctrl) {
    uint32_t d = e->dispcnt, winin = reg16(e, 0x48), winout = reg16(e, 0x4A);
    const NdsEngine2d *g = &e->nds->engine2d[e->engine];
    if (!(d & 0xE000)) {
        memset(ctrl, 0x3F, NDS_SCREEN_WIDTH);
        return;
    }
    for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
        if ((d & 0x2000) && g->window_on[0] && in_window_x(reg16(e, 0x40), x)) ctrl[x] = winin & 0x3F;
        else if ((d & 0x4000) && g->window_on[1] && in_window_x(reg16(e, 0x42), x)) ctrl[x] = (winin >> 8) & 0x3F;
        else if ((d & 0x8000) && o->window[x]) ctrl[x] = (winout >> 8) & 0x3F;
        else ctrl[x] = winout & 0x3F;
    }
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb) {
    uint32_t out = 0;
    for (int s = 0; s < 15; s += 5) {
        uint32_t v = (((a >> s) & 31) * eva + ((b >> s) & 31) * evb) >> 4;
        out |= (v > 31 ? 31 : v) << s;
    }
    return out;
}

static uint32_t fade(uint32_t a, uint32_t evy, int up) {
    uint32_t out = 0;
    for (int s = 0; s < 15; s += 5) {
        uint32_t c = (a >> s) & 31;
        out |= (up ? c + (((31 - c) * evy) >> 4) : c - ((c * evy) >> 4)) << s;
    }
    return out;
}

static uint32_t coeff(uint32_t v) { return v > 16 ? 16 : v; }

/* One engine's BG/OBJ picture for a line, as 15-bit colors. */
static void render_graphics(Nds *nds, int engine, uint32_t line, uint16_t *out) {
    Engine e;
    uint16_t bgs[4][NDS_SCREEN_WIDTH];
    ObjLine obj;
    uint8_t ctrl[NDS_SCREEN_WIDTH];
    int order[4], n = 0;
    uint32_t prio[4];
    uint32_t bldcnt, bldalpha, eva, evb, evy, effect, backdrop;
    e.nds = nds;
    e.engine = engine;
    e.line = line;
    e.io = nds->io9 + (engine ? 0x1000 : 0);
    e.dispcnt = rd32(e.io);
    e.bg = nds->vmap9[engine ? NDS_VR_BBG : NDS_VR_ABG];
    e.bg_mask = engine ? 7 : 31;
    e.obj = nds->vmap9[engine ? NDS_VR_BOBJ : NDS_VR_AOBJ];
    e.obj_mask = engine ? 7 : 15;
    e.pal = nds->palette + (engine ? 0x400 : 0);
    e.oam = nds->oam + (engine ? 0x400 : 0);
    e.mosaic = reg16(&e, 0x4C);
    backdrop = bg_color(&e, 0);

    for (int bg = 0; bg < 4; bg++) {
        int kind;
        uint32_t cnt = reg16(&e, 8 + bg * 2), mh = (e.mosaic & 15) + 1, y = line;
        prio[bg] = cnt & 3;
        if (!(e.dispcnt & (0x100u << bg))) continue;
        kind = bg_kind(&e, bg);
        if (kind == BG_NONE || kind == BG_3D) continue; /* 3D arrives with M6 */
        if ((cnt & 0x40) && kind == BG_TEXT) y -= y % (((e.mosaic >> 4) & 15) + 1);
        if (kind == BG_TEXT) text_bg(&e, bg, y, bgs[bg]);
        else affine_bg(&e, bg, kind, line, bgs[bg]);
        if ((cnt & 0x40) && mh > 1)
            for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) bgs[bg][x] = bgs[bg][x - x % mh];
        /* Insertion sort by priority, then BG number. */
        {
            int i = n++;
            while (i > 0 && prio[order[i - 1]] > prio[bg]) {
                order[i] = order[i - 1];
                i--;
            }
            order[i] = bg;
        }
    }
    render_objs(&e, &obj);
    window_line(&e, &obj, ctrl);

    bldcnt = reg16(&e, 0x50);
    bldalpha = reg16(&e, 0x52);
    eva = coeff(bldalpha & 31);
    evb = coeff((bldalpha >> 8) & 31);
    evy = coeff(reg16(&e, 0x54) & 31);
    effect = (bldcnt >> 6) & 3;
    for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
        uint32_t id[2] = {LAYER_BD, LAYER_BD}, col[2] = {backdrop, backdrop};
        int found = 0, obj_pending = obj.prio[x] != NO_PRIO && (ctrl[x] & 0x10);
        uint32_t top;
        for (int i = 0; i < n && found < 2; i++) {
            int bg = order[i];
            uint32_t v;
            if (obj_pending && obj.prio[x] <= prio[bg]) {
                id[found] = LAYER_OBJ;
                col[found++] = obj.color[x];
                obj_pending = 0;
                if (found == 2) break;
            }
            v = bgs[bg][x];
            if (!(ctrl[x] & (1u << bg)) || !(v & OPAQUE)) continue;
            id[found] = (uint32_t)bg;
            col[found++] = v;
        }
        if (obj_pending && found < 2) {
            id[found] = LAYER_OBJ;
            col[found++] = obj.color[x];
        }
        top = col[0] & 0x7FFF;
        if (id[0] == LAYER_OBJ && obj.kind[x] != OBJ_NORMAL && (bldcnt & (0x100u << id[1]))) {
            if (obj.kind[x] == OBJ_BITMAP) top = mix(top, col[1], obj.alpha[x] + 1u, 15u - obj.alpha[x]);
            else top = mix(top, col[1], eva, evb);
        } else if ((ctrl[x] & 0x20) && effect && (bldcnt & (1u << id[0]))) {
            if (effect == 1) {
                if (bldcnt & (0x100u << id[1])) top = mix(top, col[1], eva, evb);
            } else {
                top = fade(top, evy, effect == 2);
            }
        }
        out[x] = (uint16_t)top;
    }
}

/* ---- Capture, the main-memory FIFO, master brightness ---- */

static void fifo_pop_line(Nds *nds, uint16_t *out) {
    /* Main-memory display DMA moves 4 words per request, 128 words a line. */
    for (int i = 0; i < 32 && nds->mmem_count < NDS_SCREEN_WIDTH; i++)
        nds_dma_trigger(nds, 0, NDS_DMA_MAIN_DISPLAY);
    for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
        if (nds->mmem_count) {
            out[x] = nds->mmem_fifo[nds->mmem_head];
            nds->mmem_head = (nds->mmem_head + 1) & (NDS_MMEM_FIFO_SIZE - 1);
            nds->mmem_count--;
        } else {
            out[x] = 0;
        }
    }
}

static void capture_line(Nds *nds, uint32_t line, const uint16_t *graphics, const uint16_t *fifo) {
    static const uint16_t cap_w[4] = {128, 256, 256, 256}, cap_h[4] = {128, 64, 128, 192};
    uint32_t cap = rd32(nds->io9 + 0x64), dispcnt = rd32(nds->io9), size = (cap >> 20) & 3;
    uint32_t w = cap_w[size], block = (cap >> 16) & 3, source = (cap >> 29) & 3;
    uint32_t eva = coeff(cap & 31), evb = coeff((cap >> 8) & 31);
    uint32_t write = ((cap >> 18) & 3) * 0x8000 + line * w * 2;
    uint32_t read = ((cap >> 26) & 3) * 0x8000 + line * w * 2;
    uint8_t *dst = nds->vram + block * 0x20000;
    const uint8_t *vram_src = nds->vram + ((dispcnt >> 18) & 3) * 0x20000;
    if (line >= cap_h[size] || (nds->vramcnt[block] & 0x87) != 0x80) return; /* LCDC only */
    for (uint32_t x = 0; x < w; x++) {
        /* Source A: the graphics screen is opaque; the 3D screen is empty until M6. */
        uint32_t a = source == 1 || (cap & (1u << 24)) ? 0 : OPAQUE | graphics[x];
        uint32_t b = source == 0 ? 0 : (cap & (1u << 25)) ? fifo[x] : rd16(vram_src + ((read + x * 2) & 0x1FFFF));
        uint32_t v, o = (write + x * 2) & 0x1FFFF;
        if (source == 0) {
            v = a;
        } else if (source == 1) {
            v = b;
        } else {
            uint32_t aa = a >> 15, ab = b >> 15;
            v = mix(a, b, aa * eva, ab * evb) | (((aa && eva) || (ab && evb)) ? OPAQUE : 0);
        }
        dst[o] = (uint8_t)v;
        dst[o + 1] = (uint8_t)(v >> 8);
    }
}

/* 5-bit channels widen to 6 bits as X*2+1 for X > 0 (GBATEK, 3D "COLOR"), then
 * master brightness works on 6 bits; the framebuffer is XRGB8888. */
static void output_line(const uint16_t *src, uint32_t bright, uint32_t *out) {
    uint32_t factor = coeff(bright & 31), mode = (bright >> 14) & 3;
    for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) {
        uint32_t px = 0;
        for (int s = 0; s <= 10; s += 5) { /* red ends up in the high byte */
            uint32_t c = (src[x] >> s) & 31;
            c = c ? c * 2 + 1 : 0;
            if (factor && mode == 1) c += ((63 - c) * factor) >> 4;
            else if (factor && mode == 2) c -= (c * factor) >> 4;
            px = (px << 8) | (c << 2) | (c >> 4);
        }
        out[x] = px;
    }
}

static void fill(uint16_t *line, uint16_t color) {
    for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) line[x] = color;
}

void nds_render_line(Nds *nds, uint32_t line) {
    uint32_t *top = nds->framebuffer + line * NDS_FRAME_WIDTH;
    uint32_t *bottom = top + NDS_SCREEN_HEIGHT * NDS_FRAME_WIDTH;
    uint16_t a[NDS_SCREEN_WIDTH], b[NDS_SCREEN_WIDTH], fifo[NDS_SCREEN_WIDTH];
    uint32_t dispcnt = rd32(nds->io9), cap = rd32(nds->io9 + 0x64), mode = (dispcnt >> 16) & 3;
    uint32_t source = (cap >> 29) & 3;
    int capturing = nds->capturing, graphics, uses_fifo;
    if (line >= NDS_SCREEN_HEIGHT) return;

    /* Engine A, with capture. */
    graphics = mode == 1 || (capturing && !(cap & (1u << 24)) && source != 1);
    uses_fifo = mode == 3 || (capturing && (cap & (1u << 25)) && source != 0);
    if (uses_fifo) fifo_pop_line(nds, fifo);
    if (graphics) render_graphics(nds, 0, line, a);
    if (capturing) capture_line(nds, line, a, fifo);
    switch (mode) {
    case 0: fill(a, 0x7FFF); break;
    case 1: break;
    case 2: {
        const uint8_t *src = nds->vram + ((dispcnt >> 18) & 3) * 0x20000 + line * 512;
        for (uint32_t x = 0; x < NDS_SCREEN_WIDTH; x++) a[x] = (uint16_t)rd16(src + x * 2);
        break;
    }
    default: memcpy(a, fifo, sizeof a); break;
    }

    /* Engine B has display modes 0 and 1 only. */
    if (rd32(nds->io9 + 0x1000) & 0x10000) render_graphics(nds, 1, line, b);
    else fill(b, 0x7FFF);

    if (!(nds->powcnt1 & 1)) {
        for (int x = 0; x < NDS_SCREEN_WIDTH; x++) top[x] = bottom[x] = 0;
    } else {
        /* POWCNT1 bit 15 puts engine A on the top screen. */
        int swap = (nds->powcnt1 & 0x8000) != 0;
        output_line(a, rd16(nds->io9 + 0x6C), swap ? top : bottom);
        output_line(b, rd16(nds->io9 + 0x106C), swap ? bottom : top);
    }

    /* The affine reference points step by PB/PD each line. */
    for (int en = 0; en < 2; en++) {
        const uint8_t *io = nds->io9 + (en ? 0x1000 : 0);
        for (int i = 0; i < 2; i++) {
            nds->engine2d[en].ref_x[i] += (int16_t)rd16(io + 0x22 + i * 0x10);
            nds->engine2d[en].ref_y[i] += (int16_t)rd16(io + 0x26 + i * 0x10);
        }
    }
}

/* Reloads a BG2/BG3 internal reference point from its 28-bit register. */
static void reload_ref(Nds *nds, int engine, int i, int y) {
    const uint8_t *io = nds->io9 + (engine ? 0x1000 : 0) + 0x28 + i * 0x10 + y * 4;
    int32_t v = (int32_t)(rd32(io) << 4);
    v = asr(v, 4);
    if (y) nds->engine2d[engine].ref_y[i] = v;
    else nds->engine2d[engine].ref_x[i] = v;
}

static void reload_refs(Nds *nds) {
    for (int en = 0; en < 2; en++)
        for (int i = 0; i < 2; i++) reload_ref(nds, en, i, 0), reload_ref(nds, en, i, 1);
}

void nds_gpu2d_reset(Nds *nds) {
    memset(nds->engine2d, 0, sizeof nds->engine2d);
    nds->capturing = 0;
    nds->mmem_head = nds->mmem_count = 0;
}

void nds_gpu2d_line_start(Nds *nds, uint32_t line) {
    /* Windows latch on when VCOUNT's low 8 bits reach Y1 and off at Y2, in
     * every line including VBlank (GBATEK, "DS Window Glitches"). */
    for (int en = 0; en < 2; en++) {
        const uint8_t *io = nds->io9 + (en ? 0x1000 : 0);
        for (int w = 0; w < 2; w++) {
            uint32_t v = rd16(io + 0x44 + w * 2), y = line & 0xFF;
            if (y == (v >> 8)) nds->engine2d[en].window_on[w] = 1;
            if (y == (v & 0xFF)) nds->engine2d[en].window_on[w] = 0;
        }
    }
    if (line == 0) nds->capturing = (nds->io9[0x67] & 0x80) != 0;
    if (line == NDS_SCREEN_HEIGHT) {
        if (nds->capturing) nds->io9[0x67] &= 0x7F; /* capture done */
        nds->capturing = 0;
        reload_refs(nds);
    }
}

void nds_gpu2d_io_write(Nds *nds, uint32_t off, uint32_t v, uint32_t mask) {
    uint32_t reg = off & 0xFFF;
    int engine = off >= 0x1000;
    if (reg == 0x28 || reg == 0x2C || reg == 0x38 || reg == 0x3C) {
        reload_ref(nds, engine, reg >= 0x38, (reg & 4) != 0);
    } else if (off == 0x68 && mask == 0xFFFFFFFFu) {
        for (int i = 0; i < 2 && nds->mmem_count < NDS_MMEM_FIFO_SIZE; i++) {
            nds->mmem_fifo[(nds->mmem_head + nds->mmem_count) & (NDS_MMEM_FIFO_SIZE - 1)] =
                (uint16_t)(v >> (i * 16));
            nds->mmem_count++;
        }
    }
}
