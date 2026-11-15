/* Sharp SM83 CPU. Every memory access and internal delay advances the rest of the
 * machine by one M-cycle, so instruction timing comes from the access pattern. */

#include "gb_internal.h"

#include <stdio.h>

#define FZ 0x80u
#define FN 0x40u
#define FH 0x20u
#define FC 0x10u

static uint8_t rd(Gb *g, uint16_t address) {
    gb_tick(g);
    return gb_bus_read(g, address);
}

static void wr(Gb *g, uint16_t address, uint8_t value) {
    gb_tick(g);
    gb_bus_write(g, address, value);
}

static void idle(Gb *g) { gb_tick(g); }

static uint8_t fetch(Gb *g) {
    uint8_t value = rd(g, g->pc);
    g->pc++;
    return value;
}

static uint16_t fetch16(Gb *g) {
    uint8_t lo = fetch(g);
    uint8_t hi = fetch(g);
    return (uint16_t)(lo | (hi << 8));
}

static uint16_t get_bc(const Gb *g) { return (uint16_t)((g->b << 8) | g->c); }
static uint16_t get_de(const Gb *g) { return (uint16_t)((g->d << 8) | g->e); }
static uint16_t get_hl(const Gb *g) { return (uint16_t)((g->h << 8) | g->l); }
static void set_bc(Gb *g, uint16_t v) { g->b = (uint8_t)(v >> 8); g->c = (uint8_t)v; }
static void set_de(Gb *g, uint16_t v) { g->d = (uint8_t)(v >> 8); g->e = (uint8_t)v; }
static void set_hl(Gb *g, uint16_t v) { g->h = (uint8_t)(v >> 8); g->l = (uint8_t)v; }

/* rp table: BC, DE, HL, SP */
static uint16_t get_rp(const Gb *g, int i) {
    switch (i) {
    case 0: return get_bc(g);
    case 1: return get_de(g);
    case 2: return get_hl(g);
    default: return g->sp;
    }
}

static void set_rp(Gb *g, int i, uint16_t v) {
    switch (i) {
    case 0: set_bc(g, v); break;
    case 1: set_de(g, v); break;
    case 2: set_hl(g, v); break;
    default: g->sp = v; break;
    }
}

/* r8 table: B C D E H L (HL) A */
static uint8_t get_r8(Gb *g, int i) {
    switch (i) {
    case 0: return g->b;
    case 1: return g->c;
    case 2: return g->d;
    case 3: return g->e;
    case 4: return g->h;
    case 5: return g->l;
    case 6: return rd(g, get_hl(g));
    default: return g->a;
    }
}

static void set_r8(Gb *g, int i, uint8_t v) {
    switch (i) {
    case 0: g->b = v; break;
    case 1: g->c = v; break;
    case 2: g->d = v; break;
    case 3: g->e = v; break;
    case 4: g->h = v; break;
    case 5: g->l = v; break;
    case 6: wr(g, get_hl(g), v); break;
    default: g->a = v; break;
    }
}

static void push16(Gb *g, uint16_t v) {
    g->sp--;
    wr(g, g->sp, (uint8_t)(v >> 8));
    g->sp--;
    wr(g, g->sp, (uint8_t)v);
}

static uint16_t pop16(Gb *g) {
    uint8_t lo = rd(g, g->sp++);
    uint8_t hi = rd(g, g->sp++);
    return (uint16_t)(lo | (hi << 8));
}

static int condition(const Gb *g, int cc) {
    switch (cc) {
    case 0: return !(g->f & FZ);
    case 1: return (g->f & FZ) != 0;
    case 2: return !(g->f & FC);
    default: return (g->f & FC) != 0;
    }
}

static void alu(Gb *g, int op, uint8_t v) {
    unsigned a = g->a;
    unsigned carry = (g->f & FC) ? 1u : 0u;
    unsigned r;
    switch (op) {
    case 0: /* ADD */
    case 1: /* ADC */
        if (op == 0) carry = 0;
        r = a + v + carry;
        g->f = (uint8_t)(((r & 0xFF) == 0 ? FZ : 0) | (((a & 0xF) + (v & 0xF) + carry) > 0xF ? FH : 0) |
                         (r > 0xFF ? FC : 0));
        g->a = (uint8_t)r;
        break;
    case 2: /* SUB */
    case 3: /* SBC */
    case 7: /* CP */
        if (op != 3) carry = 0;
        r = a - v - carry;
        g->f = (uint8_t)(((r & 0xFF) == 0 ? FZ : 0) | FN | ((a & 0xF) < (v & 0xF) + carry ? FH : 0) |
                         (a < (unsigned)v + carry ? FC : 0));
        if (op != 7) g->a = (uint8_t)r;
        break;
    case 4: /* AND */
        g->a = (uint8_t)(a & v);
        g->f = (uint8_t)((g->a == 0 ? FZ : 0) | FH);
        break;
    case 5: /* XOR */
        g->a = (uint8_t)(a ^ v);
        g->f = g->a == 0 ? FZ : 0;
        break;
    default: /* OR */
        g->a = (uint8_t)(a | v);
        g->f = g->a == 0 ? FZ : 0;
        break;
    }
}

static uint8_t inc8(Gb *g, uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    g->f = (uint8_t)((g->f & FC) | (r == 0 ? FZ : 0) | ((v & 0xF) == 0xF ? FH : 0));
    return r;
}

static uint8_t dec8(Gb *g, uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    g->f = (uint8_t)((g->f & FC) | (r == 0 ? FZ : 0) | FN | ((v & 0xF) == 0 ? FH : 0));
    return r;
}

static uint16_t sp_plus_e(Gb *g, int8_t e) {
    unsigned sp = g->sp;
    unsigned ue = (uint8_t)e;
    g->f = (uint8_t)((((sp & 0xF) + (ue & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + ue) > 0xFF ? FC : 0));
    return (uint16_t)(sp + (int)e);
}

static uint8_t rotate_shift(Gb *g, int op, uint8_t v) {
    unsigned carry_in = (g->f & FC) ? 1u : 0u;
    unsigned r;
    unsigned carry_out;
    switch (op) {
    case 0: carry_out = v >> 7; r = (unsigned)(v << 1) | carry_out; break;          /* RLC */
    case 1: carry_out = v & 1u; r = (unsigned)(v >> 1) | (carry_out << 7); break;   /* RRC */
    case 2: carry_out = v >> 7; r = (unsigned)(v << 1) | carry_in; break;           /* RL */
    case 3: carry_out = v & 1u; r = (unsigned)(v >> 1) | (carry_in << 7); break;    /* RR */
    case 4: carry_out = v >> 7; r = (unsigned)(v << 1); break;                      /* SLA */
    case 5: carry_out = v & 1u; r = (unsigned)(v >> 1) | (v & 0x80u); break;        /* SRA */
    case 6: carry_out = 0; r = (unsigned)((v << 4) | (v >> 4)); break;              /* SWAP */
    default: carry_out = v & 1u; r = (unsigned)(v >> 1); break;                     /* SRL */
    }
    r &= 0xFF;
    g->f = (uint8_t)((r == 0 ? FZ : 0) | (carry_out ? FC : 0));
    return (uint8_t)r;
}

static void execute_cb(Gb *g) {
    uint8_t op = fetch(g);
    int reg = op & 7;
    int bit = (op >> 3) & 7;
    uint8_t v = get_r8(g, reg);
    switch (op >> 6) {
    case 0:
        set_r8(g, reg, rotate_shift(g, bit, v));
        break;
    case 1: /* BIT: (HL) form only reads */
        g->f = (uint8_t)((g->f & FC) | FH | ((v >> bit) & 1 ? 0 : FZ));
        break;
    case 2:
        set_r8(g, reg, (uint8_t)(v & ~(1u << bit)));
        break;
    default:
        set_r8(g, reg, (uint8_t)(v | (1u << bit)));
        break;
    }
}

static void lock_up(Gb *g, uint8_t op) {
    g->locked = 1;
    snprintf(g->lock_reason, sizeof g->lock_reason,
             "The program ran illegal opcode $%02X at $%04X; the CPU has stopped.",
             op, (unsigned)(g->pc - 1));
}

/* Called after the opcode-fetch M-cycle that sampled the interrupt; that cycle is
 * the first of the five the dispatch takes, and the fetched byte is discarded. */
static void dispatch_interrupt(Gb *g) {
    uint8_t pending;
    g->ime = 0;
    idle(g);
    g->sp--;
    wr(g, g->sp, (uint8_t)(g->pc >> 8));
    /* The vector is chosen after the high byte push, which may have written IE. */
    pending = (uint8_t)(g->ie & g->if_reg & 0x1F);
    g->sp--;
    wr(g, g->sp, (uint8_t)g->pc);
    if (pending == 0) {
        g->pc = 0;
    } else {
        int bit = 0;
        while (!(pending & (1u << bit))) bit++;
        g->if_reg = (uint8_t)(g->if_reg & ~(1u << bit));
        g->pc = (uint16_t)(0x40 + bit * 8);
    }
    idle(g);
}

void gb_cpu_reset(Gb *g) {
    /* DMG register state after the boot ROM hands over to the cartridge. */
    g->a = 0x01; g->f = 0xB0;
    g->b = 0x00; g->c = 0x13;
    g->d = 0x00; g->e = 0xD8;
    g->h = 0x01; g->l = 0x4D;
    g->sp = 0xFFFE;
    g->pc = 0x0100;
    g->ime = 0;
    g->ei_pending = 0;
    g->halted = 0;
    g->halt_bug = 0;
    g->locked = 0;
    g->lock_reason[0] = '\0';
}

void gb_cpu_step(Gb *g) {
    uint8_t pending;
    uint8_t op;
    int ime_before;

    if (g->locked) {
        idle(g);
        return;
    }

    /* The SM83 fetches the next opcode during the last M-cycle of the previous
     * instruction and samples interrupts at the end of that cycle. Model that as:
     * run the fetch cycle, then decide between executing and dispatching. EI takes
     * effect one instruction late, so the decision uses IME from before EI's delay
     * expires. Leaving HALT, the halted M-cycle that saw the interrupt already
     * serves as that fetch cycle. */
    if (g->halted) {
        if (!(g->ie & g->if_reg & 0x1F)) {
            idle(g);
            return;
        }
        g->halted = 0;
        ime_before = g->ime;
    } else {
        ime_before = g->ime;
        if (g->ei_pending) {
            g->ei_pending = 0;
            g->ime = 1;
        }
        gb_tick(g);
    }
    pending = (uint8_t)(g->ie & g->if_reg & 0x1F);
    if (ime_before && pending) {
        dispatch_interrupt(g);
        return;
    }
    op = gb_bus_read(g, g->pc);
    if (g->halt_bug) {
        g->halt_bug = 0; /* the byte after HALT is read twice */
    } else {
        g->pc++;
    }

    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) { /* HALT */
            if (!g->ime && (g->ie & g->if_reg & 0x1F)) {
                g->halt_bug = 1;
            } else {
                g->halted = 1;
            }
        } else {
            set_r8(g, (op >> 3) & 7, get_r8(g, op & 7));
        }
        return;
    }
    if (op >= 0x80 && op < 0xC0) {
        alu(g, (op >> 3) & 7, get_r8(g, op & 7));
        return;
    }

    switch (op) {
    case 0x00: break; /* NOP */
    case 0x10: /* STOP: no CGB speed switch on DMG; treat as a 2-byte no-op that resets DIV */
        g->pc++;
        gb_bus_write(g, 0xFF04, 0);
        break;

    case 0x01: case 0x11: case 0x21: case 0x31:
        set_rp(g, op >> 4, fetch16(g));
        break;
    case 0x02: wr(g, get_bc(g), g->a); break;
    case 0x12: wr(g, get_de(g), g->a); break;
    case 0x22: { uint16_t hl = get_hl(g); wr(g, hl, g->a); set_hl(g, (uint16_t)(hl + 1)); break; }
    case 0x32: { uint16_t hl = get_hl(g); wr(g, hl, g->a); set_hl(g, (uint16_t)(hl - 1)); break; }
    case 0x0A: g->a = rd(g, get_bc(g)); break;
    case 0x1A: g->a = rd(g, get_de(g)); break;
    case 0x2A: { uint16_t hl = get_hl(g); g->a = rd(g, hl); set_hl(g, (uint16_t)(hl + 1)); break; }
    case 0x3A: { uint16_t hl = get_hl(g); g->a = rd(g, hl); set_hl(g, (uint16_t)(hl - 1)); break; }

    case 0x03: case 0x13: case 0x23: case 0x33:
        set_rp(g, op >> 4, (uint16_t)(get_rp(g, op >> 4) + 1));
        idle(g);
        break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B:
        set_rp(g, op >> 4, (uint16_t)(get_rp(g, op >> 4) - 1));
        idle(g);
        break;
    case 0x09: case 0x19: case 0x29: case 0x39: {
        unsigned hl = get_hl(g);
        unsigned rp = get_rp(g, op >> 4);
        unsigned r = hl + rp;
        g->f = (uint8_t)((g->f & FZ) | (((hl & 0xFFF) + (rp & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0));
        set_hl(g, (uint16_t)r);
        idle(g);
        break;
    }

    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
        int r = (op >> 3) & 7;
        set_r8(g, r, inc8(g, get_r8(g, r)));
        break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int r = (op >> 3) & 7;
        set_r8(g, r, dec8(g, get_r8(g, r)));
        break;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E: {
        uint8_t n = fetch(g);
        set_r8(g, (op >> 3) & 7, n);
        break;
    }

    case 0x07: g->a = rotate_shift(g, 0, g->a); g->f &= FC; break; /* RLCA */
    case 0x0F: g->a = rotate_shift(g, 1, g->a); g->f &= FC; break; /* RRCA */
    case 0x17: g->a = rotate_shift(g, 2, g->a); g->f &= FC; break; /* RLA */
    case 0x1F: g->a = rotate_shift(g, 3, g->a); g->f &= FC; break; /* RRA */

    case 0x08: { /* LD (nn),SP */
        uint16_t addr = fetch16(g);
        wr(g, addr, (uint8_t)g->sp);
        wr(g, (uint16_t)(addr + 1), (uint8_t)(g->sp >> 8));
        break;
    }

    case 0x18: { /* JR e */
        int8_t e = (int8_t)fetch(g);
        g->pc = (uint16_t)(g->pc + e);
        idle(g);
        break;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t e = (int8_t)fetch(g);
        if (condition(g, (op >> 3) & 3)) {
            g->pc = (uint16_t)(g->pc + e);
            idle(g);
        }
        break;
    }

    case 0x27: { /* DAA */
        unsigned a = g->a;
        int carry = (g->f & FC) != 0;
        if (!(g->f & FN)) {
            if (carry || a > 0x99) { a += 0x60; carry = 1; }
            if ((g->f & FH) || (a & 0x0F) > 0x09) a += 0x06;
        } else {
            if (carry) a -= 0x60;
            if (g->f & FH) a -= 0x06;
        }
        g->a = (uint8_t)a;
        g->f = (uint8_t)((g->f & FN) | (g->a == 0 ? FZ : 0) | (carry ? FC : 0));
        break;
    }
    case 0x2F: g->a = (uint8_t)~g->a; g->f |= FN | FH; break;                       /* CPL */
    case 0x37: g->f = (uint8_t)((g->f & FZ) | FC); break;                           /* SCF */
    case 0x3F: g->f = (uint8_t)((g->f & FZ) | ((g->f & FC) ? 0 : FC)); break;       /* CCF */

    /* 0xC0 - 0xFF */
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: /* RET cc */
        idle(g);
        if (condition(g, (op >> 3) & 3)) {
            g->pc = pop16(g);
            idle(g);
        }
        break;
    case 0xC9: g->pc = pop16(g); idle(g); break;                                     /* RET */
    case 0xD9: g->pc = pop16(g); idle(g); g->ime = 1; break;                         /* RETI */
    case 0xC1: set_bc(g, pop16(g)); break;
    case 0xD1: set_de(g, pop16(g)); break;
    case 0xE1: set_hl(g, pop16(g)); break;
    case 0xF1: { uint16_t v = pop16(g); g->a = (uint8_t)(v >> 8); g->f = (uint8_t)(v & 0xF0); break; }
    case 0xC5: idle(g); push16(g, get_bc(g)); break;
    case 0xD5: idle(g); push16(g, get_de(g)); break;
    case 0xE5: idle(g); push16(g, get_hl(g)); break;
    case 0xF5: idle(g); push16(g, (uint16_t)((g->a << 8) | g->f)); break;

    case 0xC2: case 0xCA: case 0xD2: case 0xDA: { /* JP cc,nn */
        uint16_t addr = fetch16(g);
        if (condition(g, (op >> 3) & 3)) {
            g->pc = addr;
            idle(g);
        }
        break;
    }
    case 0xC3: g->pc = fetch16(g); idle(g); break;
    case 0xE9: g->pc = get_hl(g); break;

    case 0xC4: case 0xCC: case 0xD4: case 0xDC: { /* CALL cc,nn */
        uint16_t addr = fetch16(g);
        if (condition(g, (op >> 3) & 3)) {
            idle(g);
            push16(g, g->pc);
            g->pc = addr;
        }
        break;
    }
    case 0xCD: {
        uint16_t addr = fetch16(g);
        idle(g);
        push16(g, g->pc);
        g->pc = addr;
        break;
    }

    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        idle(g);
        push16(g, g->pc);
        g->pc = (uint16_t)(op & 0x38);
        break;

    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu(g, (op >> 3) & 7, fetch(g));
        break;

    case 0xCB: execute_cb(g); break;

    case 0xE0: { uint8_t n = fetch(g); wr(g, (uint16_t)(0xFF00 | n), g->a); break; }
    case 0xF0: { uint8_t n = fetch(g); g->a = rd(g, (uint16_t)(0xFF00 | n)); break; }
    case 0xE2: wr(g, (uint16_t)(0xFF00 | g->c), g->a); break;
    case 0xF2: g->a = rd(g, (uint16_t)(0xFF00 | g->c)); break;
    case 0xEA: { uint16_t addr = fetch16(g); wr(g, addr, g->a); break; }
    case 0xFA: { uint16_t addr = fetch16(g); g->a = rd(g, addr); break; }

    case 0xE8: { /* ADD SP,e */
        int8_t e = (int8_t)fetch(g);
        g->sp = sp_plus_e(g, e);
        idle(g);
        idle(g);
        break;
    }
    case 0xF8: { /* LD HL,SP+e */
        int8_t e = (int8_t)fetch(g);
        set_hl(g, sp_plus_e(g, e));
        idle(g);
        break;
    }
    case 0xF9: g->sp = get_hl(g); idle(g); break;

    case 0xF3: g->ime = 0; g->ei_pending = 0; break; /* DI */
    case 0xFB: g->ei_pending = 1; break;             /* EI */

    default: /* D3 DB DD E3 E4 EB EC ED F4 FC FD */
        lock_up(g, op);
        break;
    }
}
