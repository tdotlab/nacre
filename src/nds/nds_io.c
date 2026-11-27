/* I/O registers at 0x04000000 for both CPUs (GBATEK, "DS I/O Maps"). Accesses
 * arrive as aligned 32-bit words with a byte mask, so a byte or halfword write
 * touches only its own bytes. Registers without behavior yet (sound, the 2D
 * engines' layer registers) are stored and read back. Timers, DMA, the cart
 * bus, SPI and the RTC have files of their own. */

#include "nds_internal.h"

#include <string.h>

static inline uint32_t raw_get(const uint8_t *io, uint32_t off) {
    return (uint32_t)io[off] | ((uint32_t)io[off + 1] << 8) | ((uint32_t)io[off + 2] << 16) |
           ((uint32_t)io[off + 3] << 24);
}

static inline void raw_put(uint8_t *io, uint32_t off, uint32_t v, uint32_t mask) {
    for (int i = 0; i < 4; i++)
        if ((mask >> (i * 8)) & 0xFF) io[off + i] = (uint8_t)(v >> (i * 8));
}

static inline uint32_t merge(uint32_t old, uint32_t v, uint32_t mask) { return (old & ~mask) | (v & mask); }

static NdsArm *cpu_of(Nds *nds, int cpu) { return cpu ? &nds->arm7 : &nds->arm9; }

/* ---- Interrupts ---- */

void nds_irq_update(Nds *nds, int cpu) {
    NdsArm *c = cpu_of(nds, cpu);
    uint32_t pending = nds->ie[cpu] & nds->irf[cpu];
    c->irq_line = (nds->ime[cpu] & 1) && pending;
    if (pending) c->halted = 0; /* halt ends on IE & IF, whatever IME says */
}

void nds_irq_raise(Nds *nds, int cpu, uint32_t bits) {
    nds->irf[cpu] |= bits;
    nds_irq_update(nds, cpu);
}

/* ---- Display status ---- */

uint32_t nds_dispstat_read(const Nds *nds, int cpu) {
    uint32_t ds = nds->dispstat[cpu];
    uint32_t target = (ds >> 8) | ((ds & 0x80) << 1);
    uint32_t v = ds & 0xFFB8;
    if (nds->line >= 192 && nds->line < 262) v |= 1;
    if (nds->line_cycle >= NDS_HBLANK_CYCLE) v |= 2;
    if (nds->line == target) v |= 4;
    return v;
}

/* ---- IPC (GBATEK, "DS Inter Process Communication") ---- */

static uint32_t ipcsync_read(const Nds *nds, int cpu) {
    return ((nds->ipcsync[cpu ^ 1] >> 8) & 0xF) | (nds->ipcsync[cpu] & 0x4F00);
}

static void ipcsync_write(Nds *nds, int cpu, uint32_t v, uint32_t mask) {
    nds->ipcsync[cpu] = (uint16_t)merge(nds->ipcsync[cpu], v, mask & 0x4F00);
    if ((mask & 0x2000) && (v & 0x2000) && (nds->ipcsync[cpu ^ 1] & 0x4000))
        nds_irq_raise(nds, cpu ^ 1, NDS_IRQ_IPCSYNC);
}

static uint32_t fifocnt_read(const Nds *nds, int cpu) {
    const NdsFifo *send = &nds->fifo[cpu], *recv = &nds->fifo[cpu ^ 1];
    uint32_t v = nds->fifocnt[cpu] & 0xC404;
    if (send->count == 0) v |= 1;
    if (send->count == 16) v |= 2;
    if (recv->count == 0) v |= 0x100;
    if (recv->count == 16) v |= 0x200;
    return v;
}

static void fifocnt_write(Nds *nds, int cpu, uint32_t v, uint32_t mask) {
    uint16_t old = nds->fifocnt[cpu];
    NdsFifo *send = &nds->fifo[cpu];
    if ((mask & 0x8) && (v & 0x8) && send->count) {
        send->count = 0;
        send->head = 0;
        /* The other side's view of an empty FIFO starts over too. */
        if (nds->fifocnt[cpu] & 0x4) nds_irq_raise(nds, cpu, NDS_IRQ_FIFO_EMPTY);
    }
    nds->fifocnt[cpu] = (uint16_t)merge(old, v, mask & 0x8404);
    if ((mask & 0x4000) && (v & 0x4000)) nds->fifocnt[cpu] &= (uint16_t)~0x4000;
    /* The FIFO IRQs are edge-triggered on their condition, including enabling. */
    if (!(old & 0x4) && (nds->fifocnt[cpu] & 0x4) && send->count == 0)
        nds_irq_raise(nds, cpu, NDS_IRQ_FIFO_EMPTY);
    if (!(old & 0x400) && (nds->fifocnt[cpu] & 0x400) && nds->fifo[cpu ^ 1].count)
        nds_irq_raise(nds, cpu, NDS_IRQ_FIFO_RECV);
}

static void fifo_send(Nds *nds, int cpu, uint32_t v) {
    NdsFifo *f = &nds->fifo[cpu];
    if (!(nds->fifocnt[cpu] & 0x8000)) return;
    if (f->count == 16) {
        nds->fifocnt[cpu] |= 0x4000;
        return;
    }
    f->data[(f->head + f->count) & 15] = v;
    f->count++;
    if (f->count == 1 && (nds->fifocnt[cpu ^ 1] & 0x400)) nds_irq_raise(nds, cpu ^ 1, NDS_IRQ_FIFO_RECV);
}

static uint32_t fifo_receive(Nds *nds, int cpu) {
    NdsFifo *f = &nds->fifo[cpu ^ 1];
    uint32_t v;
    if (f->count == 0) {
        nds->fifocnt[cpu] |= 0x4000;
        return nds->fifo_last[cpu];
    }
    v = f->data[f->head];
    if (!(nds->fifocnt[cpu] & 0x8000)) return v; /* disabled: peek */
    f->head = (f->head + 1) & 15;
    f->count--;
    nds->fifo_last[cpu] = v;
    if (f->count == 0 && (nds->fifocnt[cpu ^ 1] & 0x4)) nds_irq_raise(nds, cpu ^ 1, NDS_IRQ_FIFO_EMPTY);
    return v;
}

/* ---- Maths (ARM9; GBATEK, "DS Maths") ---- */

static uint64_t raw_get64(const uint8_t *io, uint32_t off) {
    return raw_get(io, off) | ((uint64_t)raw_get(io, off + 4) << 32);
}

static void raw_put64(uint8_t *io, uint32_t off, uint64_t v) {
    raw_put(io, off, (uint32_t)v, 0xFFFFFFFFu);
    raw_put(io, off + 4, (uint32_t)(v >> 32), 0xFFFFFFFFu);
}

static void divide(Nds *nds) {
    uint8_t *io = nds->io9;
    uint32_t mode = raw_get(io, 0x280) & 3;
    uint64_t numer = raw_get64(io, 0x290), denom = raw_get64(io, 0x298);
    int64_t n, d, q, r;
    int inverts = 0;
    if (mode == 0) {
        n = (int32_t)(uint32_t)numer;
        d = (int32_t)(uint32_t)denom;
    } else if (mode == 2) {
        n = (int64_t)numer;
        d = (int64_t)denom;
    } else {
        n = (int64_t)numer;
        d = (int32_t)(uint32_t)denom;
    }
    if (d == 0) {
        q = n < 0 ? 1 : -1;
        r = n;
        inverts = mode == 0;
    } else if (mode == 0 && n == INT32_MIN && d == -1) {
        q = n;
        r = 0;
        inverts = 1;
    } else if (n == INT64_MIN && d == -1) {
        q = n;
        r = 0;
    } else {
        q = n / d;
        r = n % d;
    }
    if (inverts) q = (int64_t)((uint64_t)q ^ 0xFFFFFFFF00000000ull);
    /* DIV0 (bit 14) looks at the full 64-bit denominator, even in 32-bit mode. */
    raw_put(io, 0x280, mode | (denom == 0 ? 0x4000u : 0), 0xFFFF);
    raw_put64(io, 0x2A0, (uint64_t)q);
    raw_put64(io, 0x2A8, (uint64_t)r);
}

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ull << 62;
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
    return (uint32_t)r;
}

static void square_root(Nds *nds) {
    uint8_t *io = nds->io9;
    uint64_t param = raw_get64(io, 0x2B8);
    if (!(raw_get(io, 0x2B0) & 1)) param &= 0xFFFFFFFFu;
    raw_put(io, 0x2B4, isqrt64(param), 0xFFFFFFFFu);
}

/* ---- Keys ---- */

static uint32_t keyinput(const Nds *nds) { return 0x3FFu & ~(uint32_t)(nds->buttons & 0x3FF); }

static uint32_t extkeyin(const Nds *nds) {
    uint32_t v = 0x7F;
    if (nds->buttons & (1u << NDS_BUTTON_X)) v &= ~1u;
    if (nds->buttons & (1u << NDS_BUTTON_Y)) v &= ~2u;
    if (nds->touching) v &= ~0x40u;
    return v;
}

/* ---- Register access ---- */

uint32_t nds_io_read(Nds *nds, int cpu, uint32_t addr) {
    uint32_t off = addr - 0x04000000u;
    const uint8_t *io = cpu ? nds->io7 : nds->io9;
    if (addr == 0x04100000u) return fifo_receive(nds, cpu);
    if (addr == 0x04100010u) return nds_cart_read_data(nds, cpu);
    if (off >= NDS_IO_SIZE) return 0;
    if (off >= 0x100 && off < 0x110) return nds_timer_read(nds, cpu, off);
    if (off >= 0x1A0 && off < 0x1C0) return nds_cart_read(nds, cpu, off);
    switch (off) {
    case 0x004: return nds_dispstat_read(nds, cpu) | (nds->line << 16);
    case 0x130: return keyinput(nds) | (raw_get(io, 0x130) & 0xFFFF0000u);
    case 0x134: return cpu ? (raw_get(io, 0x134) & 0xFFFF) | (extkeyin(nds) << 16) : 0;
    case 0x180: return ipcsync_read(nds, cpu);
    case 0x184: return fifocnt_read(nds, cpu);
    case 0x188: return 0;
    case 0x138: return cpu ? nds_rtc_read(nds) : 0;
    case 0x1C0: return cpu ? nds_spi_read(nds) : 0;
    case 0x204:
        if (cpu) return (raw_get(nds->io7, 0x204) & 0x7F) | (raw_get(nds->io9, 0x204) & 0xFF80);
        return raw_get(io, 0x204) & 0xFFFF;
    case 0x208: return nds->ime[cpu];
    case 0x210: return nds->ie[cpu];
    case 0x214: return nds->irf[cpu];
    case 0x240:
        if (cpu) {
            uint32_t stat = 0;
            if ((nds->vramcnt[2] & 0x87) == 0x82) stat |= 1;
            if ((nds->vramcnt[3] & 0x87) == 0x82) stat |= 2;
            return stat | ((uint32_t)nds->wramcnt << 8);
        }
        return (uint32_t)nds->vramcnt[0] | ((uint32_t)nds->vramcnt[1] << 8) |
               ((uint32_t)nds->vramcnt[2] << 16) | ((uint32_t)nds->vramcnt[3] << 24);
    case 0x244:
        if (cpu) return 0;
        return (uint32_t)nds->vramcnt[4] | ((uint32_t)nds->vramcnt[5] << 8) |
               ((uint32_t)nds->vramcnt[6] << 16) | ((uint32_t)nds->wramcnt << 24);
    case 0x248:
        if (cpu) return 0;
        return (uint32_t)nds->vramcnt[7] | ((uint32_t)nds->vramcnt[8] << 8);
    case 0x300: return nds->postflg[cpu] | (raw_get(io, 0x300) & 0xFFFFFF00u);
    case 0x304: return cpu ? raw_get(io, 0x304) : nds->powcnt1;
    default: return raw_get(io, off);
    }
}

static void vramcnt_write(Nds *nds, int first, uint32_t v, uint32_t mask, int count) {
    int changed = 0;
    for (int i = 0; i < count; i++) {
        if (!((mask >> (i * 8)) & 0xFF)) continue;
        nds->vramcnt[first + i] = (uint8_t)(v >> (i * 8));
        changed = 1;
    }
    if (changed) nds_map_vram(nds);
}

void nds_io_write(Nds *nds, int cpu, uint32_t addr, uint32_t v, uint32_t mask) {
    uint32_t off = addr - 0x04000000u;
    uint8_t *io = cpu ? nds->io7 : nds->io9;
    if (off >= NDS_IO_SIZE) return;
    if (off >= 0xB0 && off < 0xE0) {
        nds_dma_write(nds, cpu, off, v, mask);
        return;
    }
    if (off >= 0x100 && off < 0x110) {
        nds_timer_write(nds, cpu, off, v, mask);
        return;
    }
    if (off >= 0x1A0 && off < 0x1C0) {
        nds_cart_write(nds, cpu, off, v, mask);
        return;
    }
    switch (off) {
    case 0x004:
        nds->dispstat[cpu] = (uint16_t)merge(nds->dispstat[cpu], v, mask & 0xFFB8);
        return; /* VCOUNT writes are ignored */
    case 0x130:
        raw_put(io, off, v, mask & 0xFFFF0000u); /* KEYCNT; KEYINPUT is read-only */
        return;
    case 0x138:
        if (cpu) nds_rtc_write(nds, v, mask);
        return;
    case 0x180:
        ipcsync_write(nds, cpu, v, mask);
        return;
    case 0x1C0:
        if (cpu) nds_spi_write(nds, v, mask);
        return;
    case 0x184:
        fifocnt_write(nds, cpu, v, mask);
        return;
    case 0x188:
        fifo_send(nds, cpu, v & mask);
        return;
    case 0x208:
        nds->ime[cpu] = merge(nds->ime[cpu], v, mask & 1);
        nds_irq_update(nds, cpu);
        return;
    case 0x210:
        nds->ie[cpu] = merge(nds->ie[cpu], v, mask);
        nds_irq_update(nds, cpu);
        return;
    case 0x214:
        nds->irf[cpu] &= ~(v & mask);
        nds_irq_update(nds, cpu);
        return;
    case 0x240:
        if (!cpu) vramcnt_write(nds, 0, v, mask, 4);
        return;
    case 0x244:
        if (cpu) return;
        vramcnt_write(nds, 4, v, mask & 0x00FFFFFFu, 3);
        if (mask & 0xFF000000u) {
            nds->wramcnt = (uint8_t)((v >> 24) & 3);
            nds_map_wram(nds);
        }
        return;
    case 0x248:
        if (!cpu) vramcnt_write(nds, 7, v, mask & 0xFFFF, 2);
        return;
    case 0x300:
        if (mask & 0xFF) {
            /* POSTFLG bit 0 can only be set; the ARM9 also has a R/W bit 1. */
            nds->postflg[cpu] = (uint8_t)((nds->postflg[cpu] & 1) | (v & (cpu ? 1 : 3)));
        }
        if (cpu && (mask & 0xFF00)) {
            unsigned mode = (v >> 14) & 3; /* HALTCNT */
            if (mode >= 2) nds->arm7.halted = !(nds->ie[1] & nds->irf[1]);
        }
        raw_put(io, off, v, mask & 0xFFFF0000u);
        return;
    case 0x304:
        if (cpu) raw_put(io, off, v, mask);
        else nds->powcnt1 = merge(nds->powcnt1, v, mask & 0x820F);
        return;
    default:
        break;
    }
    raw_put(io, off, v, mask);
    if (!cpu) {
        if (off == 0x280 || off == 0x290 || off == 0x294 || off == 0x298 || off == 0x29C) divide(nds);
        else if (off == 0x2B0 || off == 0x2B8 || off == 0x2BC) square_root(nds);
    }
}

void nds_io_reset(Nds *nds) {
    memset(nds->io9, 0, sizeof nds->io9);
    memset(nds->io7, 0, sizeof nds->io7);
    memset(nds->ime, 0, sizeof nds->ime);
    memset(nds->ie, 0, sizeof nds->ie);
    memset(nds->irf, 0, sizeof nds->irf);
    memset(nds->dispstat, 0, sizeof nds->dispstat);
    memset(nds->ipcsync, 0, sizeof nds->ipcsync);
    memset(nds->fifo, 0, sizeof nds->fifo);
    memset(nds->fifo_last, 0, sizeof nds->fifo_last);
    memset(nds->postflg, 0, sizeof nds->postflg);
    nds->fifocnt[0] = nds->fifocnt[1] = 0; /* the status bits are computed on read */
    nds->powcnt1 = 0;
    divide(nds);
}
