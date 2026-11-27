/* The four DMA channels of each CPU (GBATEK, "DS DMA Transfers", which builds
 * on "GBA DMA Transfers"). SAD, DAD and CNT live in the I/O array, since they
 * read back; enabling a channel latches them into its internal registers.
 * A block runs all at once, and the CPU that owns the channel is charged an
 * estimate of the bus time. ARM9 DMA goes past the TCMs to the bus. */

#include "nds_internal.h"

#include <string.h>

static uint32_t io_get(const uint8_t *io, uint32_t off) {
    return (uint32_t)io[off] | ((uint32_t)io[off + 1] << 8) | ((uint32_t)io[off + 2] << 16) |
           ((uint32_t)io[off + 3] << 24);
}

static void io_put(uint8_t *io, uint32_t off, uint32_t v, uint32_t mask) {
    for (int i = 0; i < 4; i++)
        if ((mask >> (i * 8)) & 0xFF) io[off + i] = (uint8_t)(v >> (i * 8));
}

static uint8_t *io_of(Nds *nds, int cpu) { return cpu ? nds->io7 : nds->io9; }

static uint32_t base_of(int ch) { return 0xB0u + 12u * (uint32_t)ch; }

static uint32_t cnt_of(const Nds *nds, int cpu, int ch) {
    return io_get(cpu ? nds->io7 : nds->io9, base_of(ch) + 8);
}

static int mode_of(int cpu, int ch, uint32_t cnt) {
    if (!cpu) return (int)((cnt >> 27) & 7); /* the enum follows the ARM9's order */
    switch ((cnt >> 28) & 3) {
    case 0: return NDS_DMA_IMMEDIATE;
    case 1: return NDS_DMA_VBLANK;
    case 2: return NDS_DMA_CART;
    default: return (ch & 1) ? NDS_DMA_GBA : NDS_DMA_WIFI;
    }
}

/* Word counts: 21 bits on the ARM9; 14 bits on ARM7 channels 0-2, 16 on 3.
 * Zero means the maximum. */
static uint32_t count_of(int cpu, int ch, uint32_t cnt) {
    uint32_t mask = !cpu ? 0x1FFFFFu : ch == 3 ? 0xFFFFu : 0x3FFFu;
    uint32_t n = cnt & mask;
    return n ? n : mask + 1;
}

/* ARM7 channel 0 reads, and channels 0-2 write, only the first 128 MiB. */
static uint32_t src_mask(int cpu, int ch) { return cpu && ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu; }
static uint32_t dst_mask(int cpu, int ch) { return cpu && ch != 3 ? 0x07FFFFFFu : 0x0FFFFFFFu; }

void nds_dma_reset(Nds *nds) { memset(nds->dma, 0, sizeof nds->dma); }

static void run_block(Nds *nds, int cpu, int ch) {
    NdsDma *d = &nds->dma[cpu][ch];
    NdsArm *c = cpu ? &nds->arm7 : &nds->arm9;
    uint32_t cnt = cnt_of(nds, cpu, ch);
    int word = (cnt >> 26) & 1, mode = mode_of(cpu, ch, cnt);
    uint32_t size = word ? 4u : 2u, dctl = (cnt >> 21) & 3, sctl = (cnt >> 23) & 3;
    uint32_t sstep = sctl == 1 ? 0u - size : sctl == 2 ? 0u : size; /* 3 is prohibited */
    uint32_t dstep = dctl == 1 ? 0u - size : dctl == 2 ? 0u : size;
    uint32_t n = d->count;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t s = d->src & ~(size - 1), t = d->dst & ~(size - 1);
        if (word) {
            uint32_t v = cpu ? nds7_read32(nds, s) : nds9_dma_read32(nds, s);
            if (cpu) nds7_write32(nds, t, v);
            else nds9_dma_write32(nds, t, v);
        } else {
            uint16_t v = cpu ? nds7_read16(nds, s) : nds9_dma_read16(nds, s);
            if (cpu) nds7_write16(nds, t, v);
            else nds9_dma_write16(nds, t, v);
        }
        d->src += sstep;
        d->dst += dstep;
    }
    /* About two bus cycles per halfword moved, in the owning CPU's clock. */
    c->cycles += (uint64_t)n * (word ? 4u : 2u) * (cpu ? 1u : 2u);
    /* Repeat keeps a triggered channel armed; immediate and (until the 3D
     * engine exists) geometry FIFO transfers finish. */
    if ((cnt & (1u << 25)) && mode != NDS_DMA_IMMEDIATE && mode != NDS_DMA_GXFIFO) {
        d->count = count_of(cpu, ch, cnt);
        if (dctl == 3) d->dst = io_get(io_of(nds, cpu), base_of(ch) + 4) & dst_mask(cpu, ch);
    } else {
        io_put(io_of(nds, cpu), base_of(ch) + 8, cnt & ~0x80000000u, 0xFFFFFFFFu);
    }
    if (cnt & (1u << 30)) nds_irq_raise(nds, cpu, NDS_IRQ_DMA(ch));
}

void nds_dma_write(Nds *nds, int cpu, uint32_t off, uint32_t v, uint32_t mask) {
    uint8_t *io = io_of(nds, cpu);
    int ch = (int)((off - 0xB0u) / 12u);
    uint32_t old = cnt_of(nds, cpu, ch), cnt;
    io_put(io, off, v, mask);
    if (off != base_of(ch) + 8) return;
    cnt = cnt_of(nds, cpu, ch);
    if (!(old & 0x80000000u) && (cnt & 0x80000000u)) {
        NdsDma *d = &nds->dma[cpu][ch];
        int mode = mode_of(cpu, ch, cnt);
        d->src = io_get(io, base_of(ch)) & src_mask(cpu, ch);
        d->dst = io_get(io, base_of(ch) + 4) & dst_mask(cpu, ch);
        d->count = count_of(cpu, ch, cnt);
        if (mode == NDS_DMA_IMMEDIATE || mode == NDS_DMA_GXFIFO) run_block(nds, cpu, ch);
        else if (mode == NDS_DMA_CART) nds_cart_update(nds, nds_now(nds, cpu));
    }
}

void nds_dma_trigger(Nds *nds, int cpu, int mode) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt = cnt_of(nds, cpu, ch);
        if ((cnt & 0x80000000u) && mode_of(cpu, ch, cnt) == mode) run_block(nds, cpu, ch);
    }
}

int nds_dma_waiting(const Nds *nds, int cpu, int mode) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt = cnt_of(nds, cpu, ch);
        if ((cnt & 0x80000000u) && mode_of(cpu, ch, cnt) == mode) return 1;
    }
    return 0;
}
