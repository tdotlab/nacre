/* The four timers of each CPU (GBATEK, "DS Timers"). Both CPUs' timers count
 * at 33.513982 MHz, the ARM7's clock, divided by 1, 64, 256 or 1024. Counters
 * advance lazily, when read or written and at the end of each scheduler slice;
 * the scheduler ends a slice at the next overflow that raises an IRQ, so
 * overflow IRQs land on the right cycle. */

#include "nds_internal.h"

#include <string.h>

static const unsigned prescale_shift[4] = {0, 6, 8, 10};

static int running(const NdsTimer *t) { return (t->control & 0x80) != 0; }

/* Count-up mode: ticks on the previous timer's overflow. Not for timer 0. */
static int counts_up(const NdsTimer *t, int i) { return i > 0 && (t->control & 4); }

void nds_timer_reset(Nds *nds) { memset(nds->timer, 0, sizeof nds->timer); }

/* Adds `ticks` to timer `i`, reloading on each overflow and passing overflows on
 * to a counting-up neighbour. */
static void tick(Nds *nds, int cpu, int i, uint64_t ticks) {
    NdsTimer *t = &nds->timer[cpu][i];
    uint64_t room = 0x10000u - t->counter, period, overflows;
    if (ticks < room) {
        t->counter = (uint16_t)(t->counter + ticks);
        return;
    }
    ticks -= room;
    period = 0x10000u - t->reload;
    overflows = 1 + ticks / period;
    t->counter = (uint16_t)(t->reload + ticks % period);
    if (t->control & 0x40) nds_irq_raise(nds, cpu, NDS_IRQ_TIMER(i));
    if (i < 3) {
        NdsTimer *next = &nds->timer[cpu][i + 1];
        if (running(next) && counts_up(next, i + 1)) tick(nds, cpu, i + 1, overflows);
    }
}

void nds_timer_update(Nds *nds, int cpu, uint64_t now) {
    for (int i = 0; i < 4; i++) {
        NdsTimer *t = &nds->timer[cpu][i];
        uint64_t total;
        unsigned shift;
        if (now <= t->updated) continue;
        total = t->frac + (now - t->updated);
        t->updated = now;
        if (!running(t) || counts_up(t, i)) continue;
        shift = prescale_shift[t->control & 3];
        t->frac = (uint32_t)(total & ((1u << shift) - 1));
        tick(nds, cpu, i, total >> shift);
    }
}

/* Whether an overflow of timer `i` can raise an IRQ, itself or down a chain of
 * counting-up timers. */
static int overflow_matters(const Nds *nds, int cpu, int i) {
    for (;;) {
        const NdsTimer *t = &nds->timer[cpu][i];
        if (t->control & 0x40) return 1;
        if (i == 3) return 0;
        t = &nds->timer[cpu][++i];
        if (!running(t) || !counts_up(t, i)) return 0;
    }
}

uint64_t nds_timer_next_event(const Nds *nds, int cpu) {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 4; i++) {
        const NdsTimer *t = &nds->timer[cpu][i];
        unsigned shift;
        uint64_t at;
        if (!running(t) || counts_up(t, i) || !overflow_matters(nds, cpu, i)) continue;
        shift = prescale_shift[t->control & 3];
        at = t->updated + (((uint64_t)(0x10000u - t->counter) << shift) - t->frac);
        if (at < best) best = at;
    }
    return best;
}

/* TMxCNT_L (counter on read, reload on write) and TMxCNT_H at 0x100 + 4 * i. */
uint32_t nds_timer_read(Nds *nds, int cpu, uint32_t off) {
    const NdsTimer *t = &nds->timer[cpu][(off - 0x100) >> 2];
    nds_timer_update(nds, cpu, nds_now(nds, cpu));
    return t->counter | ((uint32_t)t->control << 16);
}

void nds_timer_write(Nds *nds, int cpu, uint32_t off, uint32_t v, uint32_t mask) {
    NdsTimer *t = &nds->timer[cpu][(off - 0x100) >> 2];
    nds_timer_update(nds, cpu, nds_now(nds, cpu));
    /* The reload goes first, so a 32-bit write that also starts the timer
     * starts it from the new reload, as GBATEK describes. */
    if (mask & 0xFFFF) t->reload = (uint16_t)((t->reload & ~mask) | (v & mask));
    if (mask & 0xFFFF0000u) {
        uint16_t old = t->control, hi = (uint16_t)(mask >> 16);
        t->control = (uint16_t)(((old & ~hi) | ((v >> 16) & hi)) & 0xC7);
        if (!(old & 0x80) && (t->control & 0x80)) {
            t->counter = t->reload;
            t->frac = 0;
        }
    }
}
