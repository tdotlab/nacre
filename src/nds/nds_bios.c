/* Our own DS BIOS, written from GBATEK ("BIOS Functions", "DS Cartridge Header",
 * "BIOS RAM Usage"), and direct boot. No Nintendo code is used: the two BIOS
 * images hold only exception vectors and the IRQ trampoline that calls the
 * game's handler, and the SWIs run here in C. */

#include "nds_internal.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* IRQ entry: save the scratch registers, call the handler the game stored at
 * the end of DTCM (ARM9) or ARM7 WRAM (via the 0x03FFFFFC mirror), return. */
static const uint32_t irq9[] = {
    0xE92D500F, /* stmfd sp!,{r0-r3,r12,lr} */
    0xEE190F11, /* mrc p15,0,r0,c9,c1,0  ; DTCM base */
    0xE1A00620, /* mov r0,r0,lsr #12 */
    0xE1A00600, /* mov r0,r0,lsl #12 */
    0xE2800901, /* add r0,r0,#0x4000 */
    0xE5100004, /* ldr r0,[r0,#-4] */
    0xE28FE000, /* add lr,pc,#0 */
    0xE12FFF10, /* bx r0 */
    0xE8BD500F, /* ldmfd sp!,{r0-r3,r12,lr} */
    0xE25EF004, /* subs pc,lr,#4 */
};
static const uint32_t irq7[] = {
    0xE92D500F, /* stmfd sp!,{r0-r3,r12,lr} */
    0xE3A00301, /* mov r0,#0x04000000 */
    0xE5100004, /* ldr r0,[r0,#-4] */
    0xE28FE000, /* add lr,pc,#0 */
    0xE12FFF10, /* bx r0 */
    0xE8BD500F, /* ldmfd sp!,{r0-r3,r12,lr} */
    0xE25EF004, /* subs pc,lr,#4 */
};

static void build_image(uint8_t *bios, const uint32_t *irq, size_t irq_words) {
    /* Vectors: everything but IRQ loops in place (SWIs never reach the vector). */
    for (int v = 0; v < 8; v++) put32(bios + v * 4, 0xEAFFFFFEu); /* b . */
    put32(bios + 0x18, 0xEA000000u);                               /* b 0x20 */
    for (size_t i = 0; i < irq_words; i++) put32(bios + 0x20 + i * 4, irq[i]);
    put32(bios + 0x100, 0xEAFFFFFEu); /* where guest calls from SWIs return to */
}

void nds_bios_build(Nds *nds) {
    memset(nds->bios9, 0, sizeof nds->bios9);
    memset(nds->bios7, 0, sizeof nds->bios7);
    build_image(nds->bios9, irq9, sizeof irq9 / sizeof irq9[0]);
    build_image(nds->bios7, irq7, sizeof irq7 / sizeof irq7[0]);
}

/* ---- Direct boot: the state the firmware leaves when it starts a cartridge
 * (GBATEK, "DS Cartridge Header" and "BIOS RAM Usage"). ---- */

static void boot_cpu(NdsArm *c, uint32_t entry, uint32_t sp_sys, uint32_t sp_irq, uint32_t sp_svc) {
    nds_arm_set_cpsr(c, ARM_MODE_SYS);
    memset(c->r, 0, sizeof c->r);
    nds_arm_set_banked_sp(c, ARM_MODE_SYS, sp_sys);
    nds_arm_set_banked_sp(c, ARM_MODE_IRQ, sp_irq);
    nds_arm_set_banked_sp(c, ARM_MODE_SVC, sp_svc);
    c->r[12] = entry;
    c->r[14] = entry;
    c->halted = 0;
    nds_arm_jump_bx(c, entry);
    c->branched = 0;
}

void nds_direct_boot(Nds *nds) {
    const NdsHeader *h = &nds->header;
    uint32_t mb = (0x20000u << (h->capacity & 15)) >> 20, chip_id = 0xC2u | ((mb ? mb - 1 : 0) << 8);
    size_t header_bytes = nds->rom_size < 0x170 ? nds->rom_size : 0x170;

    nds->wramcnt = 3; /* all shared WRAM to the ARM7 */
    nds_map_wram(nds);
    nds_cp15_write(nds, 9, 1, 0, 0, 0x027C000Au); /* DTCM 16 KiB at 0x027C0000 */
    nds_cp15_write(nds, 9, 1, 0, 1, 0x00000020u); /* ITCM at 0, 32 MiB virtual */
    nds_cp15_write(nds, 1, 0, 0, 0, 0x0005307Du); /* PU, caches, high vectors, TCMs on */

    for (uint32_t i = 0; i < h->arm9_size; i++)
        nds9_write8(nds, h->arm9_address + i, nds->rom[h->arm9_offset + i]);
    for (uint32_t i = 0; i < h->arm7_size; i++)
        nds7_write8(nds, h->arm7_address + i, nds->rom[h->arm7_offset + i]);
    for (size_t i = 0; i < header_bytes; i++) nds9_write8(nds, 0x027FFE00u + (uint32_t)i, nds->rom[i]);

    nds9_write32(nds, 0x027FF800u, chip_id);
    nds9_write32(nds, 0x027FF804u, chip_id);
    nds9_write32(nds, 0x027FFC00u, chip_id);
    nds9_write32(nds, 0x027FFC04u, chip_id);
    nds9_write16(nds, 0x027FFC10u, 0x5835);
    nds9_write16(nds, 0x027FFC40u, 1); /* booted from a cartridge */
    nds7_write32(nds, 0x0380F980u, 0xFBDD37BBu);

    nds->postflg[0] = nds->postflg[1] = 1;
    nds->powcnt1 = 0x820F;

    boot_cpu(&nds->arm9, h->arm9_entry, 0x03002F7Cu, 0x03003F80u, 0x03003FC0u);
    boot_cpu(&nds->arm7, h->arm7_entry, 0x0380FD80u, 0x0380FF80u, 0x0380FFC0u);
}

/* ---- Helpers for SWIs ---- */

static uint32_t rd32(NdsArm *c, uint32_t a) { return nds_bus_read32(c, a); }
static uint32_t rd16(NdsArm *c, uint32_t a) { return nds_bus_read16(c, a); }
static uint32_t rd8(NdsArm *c, uint32_t a) { return nds_bus_read8(c, a); }
static void wr32(NdsArm *c, uint32_t a, uint32_t v) { nds_bus_write32(c, a, v); }
static void wr16(NdsArm *c, uint32_t a, uint32_t v) { nds_bus_write16(c, a, (uint16_t)v); }
static void wr8(NdsArm *c, uint32_t a, uint32_t v) { nds_bus_write8(c, a, (uint8_t)v); }

/* Calls guest code at `func` (bit 0 = Thumb) with r0-r2, returning r0. Used for
 * the decompression callbacks. Registers and state are restored afterwards. */
static uint32_t guest_call(NdsArm *c, uint32_t func, uint32_t r0, uint32_t r1, uint32_t r2) {
    uint32_t saved[16], cpsr = c->cpsr, result;
    uint32_t ret = c->v5 ? NDS_BIOS9_CALL_RETURN : NDS_BIOS7_CALL_RETURN;
    long guard = 4000000;
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = r0;
    c->r[1] = r1;
    c->r[2] = r2;
    c->r[13] -= 0x40; /* stay clear of the SWI caller's stack frame */
    c->r[14] = ret;
    nds_arm_jump_bx(c, func);
    while (nds_arm_pc(c) != ret && (c->cpsr & ARM_FLAG_T || nds_arm_pc(c) != ret) && guard-- > 0)
        nds_arm_step(c);
    result = c->r[0];
    nds_arm_set_cpsr(c, cpsr);
    c->cpsr = cpsr;
    memcpy(c->r, saved, sizeof saved);
    c->branched = 0;
    return result;
}

/* Decompression source: memory, or the guest's callback structure. */
typedef struct {
    NdsArm *c;
    uint32_t addr;
    uint32_t callbacks; /* 0 for a plain memory read */
} Source;

static uint32_t src8(Source *s) {
    uint32_t v;
    if (s->callbacks) v = guest_call(s->c, rd32(s->c, s->callbacks + 8), s->addr, 0, 0) & 0xFF;
    else v = rd8(s->c, s->addr);
    s->addr++;
    return v;
}

static uint32_t src32(Source *s) {
    uint32_t v;
    if (s->callbacks) v = guest_call(s->c, rd32(s->c, s->callbacks + 16), s->addr, 0, 0);
    else v = rd32(s->c, s->addr);
    s->addr += 4;
    return v;
}

/* Opens the source and returns the header word (negative is an error). */
static int32_t src_open(Source *s, uint32_t dest, uint32_t param) {
    uint32_t header;
    if (s->callbacks) header = guest_call(s->c, rd32(s->c, s->callbacks), s->addr, dest, param);
    else header = rd32(s->c, s->addr);
    s->addr += 4;
    return (int32_t)header;
}

static void src_close(Source *s) {
    uint32_t close;
    if (!s->callbacks) return;
    close = rd32(s->c, s->callbacks + 4);
    if (close) guest_call(s->c, close, s->addr, 0, 0);
}

/* Decompression destination, written in 8- or 16-bit units. */
typedef struct {
    NdsArm *c;
    uint32_t addr;
    int wide;
    uint32_t pending; /* the low byte of a halfword not yet written */
} Dest;

static void dst_put(Dest *d, uint32_t b) {
    if (!d->wide) {
        wr8(d->c, d->addr, b);
    } else if (!(d->addr & 1)) {
        d->pending = b & 0xFF;
    } else {
        wr16(d->c, d->addr - 1, d->pending | ((b & 0xFF) << 8));
    }
    d->addr++;
}

/* The byte `back` positions behind the write position. */
static uint32_t dst_get(Dest *d, uint32_t back) {
    uint32_t a = d->addr - back;
    if (d->wide && (d->addr & 1) && a == d->addr - 1) return d->pending;
    return rd8(d->c, a);
}

static void lz77(NdsArm *c, int wide, int callback) {
    Source s = {c, c->r[0], callback ? c->r[3] : 0};
    Dest d = {c, c->r[1], wide, 0};
    int32_t header = src_open(&s, c->r[1], c->r[2]);
    uint32_t remaining;
    if (header < 0) {
        c->r[0] = (uint32_t)header;
        return;
    }
    remaining = (uint32_t)header >> 8;
    while (remaining) {
        uint32_t flags = src8(&s);
        for (int i = 0; i < 8 && remaining; i++, flags <<= 1) {
            if (flags & 0x80) {
                uint32_t b0 = src8(&s), b1 = src8(&s);
                uint32_t len = (b0 >> 4) + 3, disp = (((b0 & 0xF) << 8) | b1) + 1;
                while (len-- && remaining) {
                    dst_put(&d, dst_get(&d, disp));
                    remaining--;
                }
            } else {
                dst_put(&d, src8(&s));
                remaining--;
            }
        }
    }
    src_close(&s);
    c->r[0] = (uint32_t)header >> 8;
}

static void run_length(NdsArm *c, int wide, int callback) {
    Source s = {c, c->r[0], callback ? c->r[3] : 0};
    Dest d = {c, c->r[1], wide, 0};
    int32_t header = src_open(&s, c->r[1], c->r[2]);
    uint32_t remaining;
    if (header < 0) {
        c->r[0] = (uint32_t)header;
        return;
    }
    remaining = (uint32_t)header >> 8;
    while (remaining) {
        uint32_t flag = src8(&s);
        if (flag & 0x80) {
            uint32_t len = (flag & 0x7F) + 3, b = src8(&s);
            while (len-- && remaining) {
                dst_put(&d, b);
                remaining--;
            }
        } else {
            uint32_t len = (flag & 0x7F) + 1;
            while (len-- && remaining) {
                dst_put(&d, src8(&s));
                remaining--;
            }
        }
    }
    src_close(&s);
    c->r[0] = (uint32_t)header >> 8;
}

static void huffman(NdsArm *c) {
    Source s = {c, c->r[0], c->r[3]};
    uint8_t tree[512];
    int32_t header = src_open(&s, c->r[1], c->r[2]);
    uint32_t size, bits, dst = c->r[1], out = 0, out_bits = 0, pos = 1, tree_len;
    if (header < 0) {
        c->r[0] = (uint32_t)header;
        return;
    }
    size = (uint32_t)header >> 8;
    bits = (uint32_t)header & 15;
    if (bits != 4 && bits != 8) bits = 8;
    tree[0] = (uint8_t)src8(&s);
    tree_len = ((uint32_t)tree[0] + 1) * 2;
    for (uint32_t i = 1; i < tree_len; i++) tree[i] = (uint8_t)src8(&s);
    while (size) {
        uint32_t word = src32(&s);
        for (int i = 0; i < 32 && size; i++, word <<= 1) {
            uint32_t node = tree[pos], bit = word >> 31;
            uint32_t next = (pos & ~1u) + (node & 0x3F) * 2 + 2 + bit;
            if (next >= tree_len) next = tree_len - 1;
            if (node & (bit ? 0x40 : 0x80)) {
                out |= (uint32_t)tree[next] << out_bits;
                out_bits += bits;
                pos = 1;
                if (out_bits == 32) {
                    wr32(c, dst, out);
                    dst += 4;
                    out = out_bits = 0;
                    size = size > 4 ? size - 4 : 0;
                }
            } else {
                pos = next;
            }
        }
    }
    src_close(&s);
    c->r[0] = (uint32_t)header >> 8;
}

static void bit_unpack(NdsArm *c) {
    uint32_t src = c->r[0], dst = c->r[1], info = c->r[2];
    uint32_t len = rd16(c, info), sw = rd8(c, info + 2), dw = rd8(c, info + 3), off = rd32(c, info + 4);
    uint32_t zero = off >> 31, out = 0, out_bits = 0, smask, dmask;
    off &= 0x7FFFFFFFu;
    if (!sw || sw > 8 || !dw || dw > 32) return;
    smask = (1u << sw) - 1;
    dmask = dw == 32 ? 0xFFFFFFFFu : (1u << dw) - 1;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t b = rd8(c, src + i);
        for (uint32_t bit = 0; bit < 8; bit += sw) {
            uint32_t unit = (b >> bit) & smask;
            if (unit || zero) unit += off;
            out |= (unit & dmask) << out_bits;
            out_bits += dw;
            if (out_bits >= 32) {
                wr32(c, dst, out);
                dst += 4;
                out = out_bits = 0;
            }
        }
    }
}

static void diff_unfilter(NdsArm *c, int wide) {
    uint32_t src = c->r[0], dst = c->r[1], size = rd32(c, src) >> 8, prev = 0;
    src += 4;
    if (wide) {
        for (uint32_t i = 0; i + 1 < size + 1; i += 2) {
            prev = (prev + rd16(c, src + i)) & 0xFFFF;
            wr16(c, dst + i, prev);
        }
    } else {
        for (uint32_t i = 0; i < size; i++) {
            prev = (prev + rd8(c, src + i)) & 0xFF;
            wr8(c, dst + i, prev);
        }
    }
}

static void cpu_set(NdsArm *c, int fast) {
    uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2], count = ctl & 0x1FFFFF;
    int fill = (ctl >> 24) & 1, wide = fast || ((ctl >> 26) & 1);
    if (fast) count = (count + 7) & ~7u;
    if (wide) {
        uint32_t v = 0;
        src &= ~3u;
        dst &= ~3u;
        if (fill) v = rd32(c, src);
        for (uint32_t i = 0; i < count; i++) wr32(c, dst + i * 4, fill ? v : rd32(c, src + i * 4));
    } else {
        uint32_t v = 0;
        src &= ~1u;
        dst &= ~1u;
        if (fill) v = rd16(c, src);
        for (uint32_t i = 0; i < count; i++) wr16(c, dst + i * 2, fill ? v : rd16(c, src + i * 2));
    }
    c->cycles += count;
}

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static void halt_cpu(NdsArm *c) {
    Nds *nds = c->nds;
    int i = c->v5 ? 0 : 1;
    c->halted = !(nds->ie[i] & nds->irf[i]);
}

/* IntrWait: waits until one of the flags in r1 shows up in the check word the
 * game's IRQ handler maintains, then clears it. The SWI stays in place (r15
 * unchanged) while waiting, so it runs again after each interrupt returns. */
static void intr_wait(NdsArm *c) {
    Nds *nds = c->nds;
    int i = c->v5 ? 0 : 1;
    uint32_t check = c->v5 ? nds->cp15.dtcm_base + 0x3FF8 : 0x0380FFF8u, flags;
    nds->ime[i] = 1;
    nds_irq_update(nds, i);
    if (!c->intr_waiting) {
        c->intr_waiting = 1;
        if (c->r[0] & 1) wr32(c, check, rd32(c, check) & ~c->r[1]);
        /* The ARM9 BIOS always waits for at least one interrupt. */
        if (c->v5) goto wait;
    }
    flags = rd32(c, check);
    if (flags & c->r[1]) {
        wr32(c, check, flags & ~c->r[1]);
        c->intr_waiting = 0;
        return;
    }
wait:
    halt_cpu(c);
    c->branched = 1; /* re-execute this SWI */
}

static void soft_reset(NdsArm *c) {
    uint32_t base = c->v5 ? c->nds->cp15.dtcm_base + 0x3E00 : 0x0380FE00u;
    uint32_t entry = rd32(c, c->v5 ? 0x027FFE24u : 0x027FFE34u);
    for (uint32_t i = 0; i < 0x200; i += 4) wr32(c, base + i, 0);
    nds_arm_set_cpsr(c, ARM_MODE_SYS);
    memset(c->r, 0, sizeof c->r);
    if (c->v5) {
        nds_arm_set_banked_sp(c, ARM_MODE_SVC, 0x00803FC0u);
        nds_arm_set_banked_sp(c, ARM_MODE_IRQ, 0x00803FA0u);
        nds_arm_set_banked_sp(c, ARM_MODE_SYS, 0x00803EC0u);
    } else {
        nds_arm_set_banked_sp(c, ARM_MODE_SVC, 0x0380FFDCu);
        nds_arm_set_banked_sp(c, ARM_MODE_IRQ, 0x0380FFB0u);
        nds_arm_set_banked_sp(c, ARM_MODE_SYS, 0x0380FF00u);
    }
    nds_arm_set_banked_lr(c, ARM_MODE_SVC, 0);
    nds_arm_set_banked_lr(c, ARM_MODE_IRQ, 0);
    c->intr_waiting = 0;
    nds_arm_jump_bx(c, entry);
}

void nds_bios_swi(NdsArm *c, uint32_t number) {
    int arm9 = c->v5;
    c->cycles += 8;
    if (number != 0x04 && number != 0x05) c->intr_waiting = 0;
    switch (number) {
    case 0x00:
        soft_reset(c);
        break;
    case 0x03: /* WaitByLoop */
        c->cycles += (uint64_t)c->r[0] * 4;
        c->r[0] = 0;
        break;
    case 0x05: /* VBlankIntrWait */
        c->r[0] = 1;
        c->r[1] = 1;
        /* fall through */
    case 0x04:
        intr_wait(c);
        break;
    case 0x06: /* Halt */
        halt_cpu(c);
        if (arm9) c->r[0] = 0;
        break;
    case 0x07: /* Sleep (ARM7) */
        if (!arm9) halt_cpu(c);
        break;
    case 0x08: /* SoundBias (ARM7): sound arrives in M4 */
        break;
    case 0x09: { /* Div */
        int32_t n = (int32_t)c->r[0], d = (int32_t)c->r[1], q, r;
        if (d == 0) {
            q = n < 0 ? 1 : -1;
            r = n;
        } else if (n == INT32_MIN && d == -1) {
            q = n;
            r = 0;
        } else {
            q = n / d;
            r = n % d;
        }
        c->r[0] = (uint32_t)q;
        c->r[1] = (uint32_t)r;
        c->r[3] = (uint32_t)(q < 0 ? -(int64_t)q : q);
        break;
    }
    case 0x0B:
        cpu_set(c, 0);
        break;
    case 0x0C:
        cpu_set(c, 1);
        break;
    case 0x0D:
        c->r[0] = isqrt32(c->r[0]);
        break;
    case 0x0E: { /* GetCRC16 */
        uint16_t crc = (uint16_t)c->r[0];
        uint32_t len = c->r[2] & ~1u;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t b = (uint8_t)rd8(c, c->r[1] + i);
            crc = nds_crc16(crc, &b, 1);
        }
        c->r[0] = crc;
        break;
    }
    case 0x0F: /* IsDebugger */
        c->r[0] = 0;
        break;
    case 0x10:
        bit_unpack(c);
        break;
    case 0x11:
        lz77(c, 0, 0);
        break;
    case 0x12:
        lz77(c, 1, 1);
        break;
    case 0x13:
        huffman(c);
        break;
    case 0x14:
        run_length(c, 0, 0);
        break;
    case 0x15:
        run_length(c, 1, 1);
        break;
    case 0x16:
        if (arm9) diff_unfilter(c, 0);
        break;
    case 0x18:
        if (arm9) diff_unfilter(c, 1);
        break;
    case 0x1F:
        if (arm9) {
            nds_io_write(c->nds, 0, 0x04000300u, c->r[0], 0xFF); /* CustomPost */
        } else if (((c->r[2] >> 6) & 3) >= 2) {
            halt_cpu(c); /* CustomHalt */
        }
        break;
    default:
        /* The ARM7 sound tables (0x1A-0x1C) come with sound in M4. */
        break;
    }
}
