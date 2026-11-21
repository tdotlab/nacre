/* ARM/Thumb interpreter for both DS CPUs, from GBATEK's "ARM CPU Reference".
 * The ARM9 is ARMv5TE (v5 = 1), the ARM7 ARMv4T. Where the two differ, the
 * difference is marked. r[15] always reads as the executing instruction's address
 * plus 8 (ARM) or 4 (Thumb); every write to r15 goes through nds_arm_jump*, which
 * sets `branched` so the step loop does not advance it. */

#include "nds_internal.h"

#include <string.h>

#define FLAG_N ARM_FLAG_N
#define FLAG_Z ARM_FLAG_Z
#define FLAG_C ARM_FLAG_C
#define FLAG_V ARM_FLAG_V
#define FLAG_Q ARM_FLAG_Q
#define FLAG_T ARM_FLAG_T

/* Condition results by condition code and NZCV nibble. */
static uint16_t cond_table[16];
static int cond_table_ready;

static void build_cond_table(void) {
    for (int cond = 0; cond < 16; cond++) {
        uint16_t bits = 0;
        for (int f = 0; f < 16; f++) {
            int n = (f >> 3) & 1, z = (f >> 2) & 1, c = (f >> 1) & 1, v = f & 1, ok;
            switch (cond) {
            case 0x0: ok = z; break;
            case 0x1: ok = !z; break;
            case 0x2: ok = c; break;
            case 0x3: ok = !c; break;
            case 0x4: ok = n; break;
            case 0x5: ok = !n; break;
            case 0x6: ok = v; break;
            case 0x7: ok = !v; break;
            case 0x8: ok = c && !z; break;
            case 0x9: ok = !c || z; break;
            case 0xA: ok = n == v; break;
            case 0xB: ok = n != v; break;
            case 0xC: ok = !z && n == v; break;
            case 0xD: ok = z || n != v; break;
            case 0xE: ok = 1; break;
            default: ok = 0; break;
            }
            if (ok) bits |= (uint16_t)(1u << f);
        }
        cond_table[cond] = bits;
    }
    cond_table_ready = 1;
}

static inline int cond_passed(uint32_t cpsr, uint32_t cond) {
    return (cond_table[cond] >> (cpsr >> 28)) & 1;
}

static inline uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline int popcount16(uint32_t v) {
    int n = 0;
    for (v &= 0xFFFF; v; v &= v - 1) n++;
    return n;
}

/* ---- Registers and modes ---- */

static int bank_of(uint32_t mode) {
    switch (mode & 0x1F) {
    case ARM_MODE_FIQ: return 1;
    case ARM_MODE_IRQ: return 2;
    case ARM_MODE_SVC: return 3;
    case ARM_MODE_ABT: return 4;
    case ARM_MODE_UND: return 5;
    default: return 0;
    }
}

void nds_arm_set_cpsr(NdsArm *c, uint32_t value) {
    int ob = bank_of(c->cpsr), nb;
    value |= 0x10; /* mode bit 4 is always set on these CPUs */
    nb = bank_of(value);
    if (ob != nb) {
        c->bank_r13[ob] = c->r[13];
        c->bank_r14[ob] = c->r[14];
        c->bank_spsr[ob] = c->spsr;
        if (ob == 1) {
            memcpy(c->fiq_r8, &c->r[8], sizeof c->fiq_r8);
            memcpy(&c->r[8], c->usr_r8, sizeof c->usr_r8);
        } else if (nb == 1) {
            memcpy(c->usr_r8, &c->r[8], sizeof c->usr_r8);
            memcpy(&c->r[8], c->fiq_r8, sizeof c->fiq_r8);
        }
        c->r[13] = c->bank_r13[nb];
        c->r[14] = c->bank_r14[nb];
        c->spsr = c->bank_spsr[nb];
    }
    c->cpsr = value;
}

void nds_arm_set_banked_sp(NdsArm *c, uint32_t mode, uint32_t sp) {
    int b = bank_of(mode);
    if (b == bank_of(c->cpsr)) c->r[13] = sp;
    else c->bank_r13[b] = sp;
}

void nds_arm_set_banked_lr(NdsArm *c, uint32_t mode, uint32_t lr) {
    int b = bank_of(mode);
    if (b == bank_of(c->cpsr)) c->r[14] = lr;
    else c->bank_r14[b] = lr;
}

/* User-bank registers, for LDM/STM with the S bit. */
static uint32_t user_reg(NdsArm *c, int i) {
    int b = bank_of(c->cpsr);
    if (i >= 8 && i <= 12 && b == 1) return c->usr_r8[i - 8];
    if (i == 13 && b != 0) return c->bank_r13[0];
    if (i == 14 && b != 0) return c->bank_r14[0];
    return c->r[i];
}

static void set_user_reg(NdsArm *c, int i, uint32_t v) {
    int b = bank_of(c->cpsr);
    if (i >= 8 && i <= 12 && b == 1) c->usr_r8[i - 8] = v;
    else if (i == 13 && b != 0) c->bank_r13[0] = v;
    else if (i == 14 && b != 0) c->bank_r14[0] = v;
    else c->r[i] = v;
}

void nds_arm_jump(NdsArm *c, uint32_t address) {
    if (c->cpsr & FLAG_T) c->r[15] = (address & ~1u) + 4;
    else c->r[15] = (address & ~3u) + 8;
    c->branched = 1;
}

void nds_arm_jump_bx(NdsArm *c, uint32_t address) {
    if (address & 1) c->cpsr |= FLAG_T;
    else c->cpsr &= ~FLAG_T;
    nds_arm_jump(c, address);
}

uint32_t nds_arm_pc(const NdsArm *c) { return c->r[15] - ((c->cpsr & FLAG_T) ? 4 : 8); }

/* Address of the instruction after the executing one. */
static inline uint32_t next_pc(const NdsArm *c) { return c->r[15] - ((c->cpsr & FLAG_T) ? 2 : 4); }

void nds_arm_exception(NdsArm *c, uint32_t mode, uint32_t vector, uint32_t lr) {
    uint32_t old = c->cpsr;
    nds_arm_set_cpsr(c, (old & ~0x3Fu) | mode | ARM_FLAG_I);
    c->spsr = old;
    c->r[14] = lr;
    nds_arm_jump(c, c->vector_base + vector);
}

static void undefined(NdsArm *c) {
    c->undefined_count++;
    c->last_undefined = nds_arm_pc(c);
    nds_arm_exception(c, ARM_MODE_UND, 0x04, next_pc(c));
}

void nds_arm_reset(NdsArm *c, struct Nds *nds, int v5) {
    if (!cond_table_ready) build_cond_table();
    memset(c, 0, sizeof *c);
    c->nds = nds;
    c->v5 = v5;
    c->cpsr = ARM_MODE_SVC | ARM_FLAG_I | ARM_FLAG_F;
    c->vector_base = v5 ? 0xFFFF0000u : 0;
    c->r[15] = c->vector_base + 8;
}

/* ---- Memory, with an approximate cost of one cycle per access ---- */

static inline uint32_t ld32(NdsArm *c, uint32_t a) { c->cycles++; return nds_bus_read32(c, a); }
static inline uint32_t ld16(NdsArm *c, uint32_t a) { c->cycles++; return nds_bus_read16(c, a); }
static inline uint32_t ld8(NdsArm *c, uint32_t a) { c->cycles++; return nds_bus_read8(c, a); }
static inline void st32(NdsArm *c, uint32_t a, uint32_t v) { c->cycles++; nds_bus_write32(c, a, v); }
static inline void st16(NdsArm *c, uint32_t a, uint32_t v) { c->cycles++; nds_bus_write16(c, a, (uint16_t)v); }
static inline void st8(NdsArm *c, uint32_t a, uint32_t v) { c->cycles++; nds_bus_write8(c, a, (uint8_t)v); }

/* ---- Flags and the barrel shifter ---- */

static inline void set_nz(NdsArm *c, uint32_t r) {
    c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z)) | (r & FLAG_N) | (r ? 0 : FLAG_Z);
}

static inline void set_nzc(NdsArm *c, uint32_t r, int carry) {
    c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C)) | (r & FLAG_N) | (r ? 0 : FLAG_Z) |
              (carry ? FLAG_C : 0);
}

static inline void set_nzcv(NdsArm *c, uint32_t r, int carry, int overflow) {
    c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) | (r & FLAG_N) | (r ? 0 : FLAG_Z) |
              (carry ? FLAG_C : 0) | (overflow ? FLAG_V : 0);
}

static inline int carry_flag(const NdsArm *c) { return (c->cpsr >> 29) & 1; }

/* a + b + cin */
static uint32_t alu_add(NdsArm *c, uint32_t a, uint32_t b, uint32_t cin, int s) {
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)wide;
    if (s) set_nzcv(c, r, (int)(wide >> 32), (int)((~(a ^ b) & (a ^ r)) >> 31));
    return r;
}

/* a - b - (1 - cin); carry means no borrow */
static uint32_t alu_sub(NdsArm *c, uint32_t a, uint32_t b, uint32_t cin, int s) {
    uint32_t borrow = 1 - cin;
    uint32_t r = a - b - borrow;
    if (s) set_nzcv(c, r, (uint64_t)a >= (uint64_t)b + borrow, (int)(((a ^ b) & (a ^ r)) >> 31));
    return r;
}

/* Shift by an immediate amount: #0 means LSL #0, LSR #32, ASR #32, RRX. */
static uint32_t shift_imm(int type, uint32_t v, unsigned amount, int *carry) {
    switch (type) {
    case 0:
        if (amount) {
            *carry = (v >> (32 - amount)) & 1;
            v <<= amount;
        }
        return v;
    case 1:
        if (!amount) {
            *carry = v >> 31;
            return 0;
        }
        *carry = (v >> (amount - 1)) & 1;
        return v >> amount;
    case 2:
        if (!amount) {
            *carry = v >> 31;
            return (uint32_t)((int32_t)v >> 31);
        }
        *carry = (v >> (amount - 1)) & 1;
        return (uint32_t)((int32_t)v >> amount);
    default:
        if (!amount) {
            int out = v & 1;
            v = (v >> 1) | ((uint32_t)*carry << 31);
            *carry = out;
            return v;
        }
        *carry = (v >> (amount - 1)) & 1;
        return ror32(v, amount);
    }
}

/* Shift by a register: only the low byte counts, and 0 changes nothing. */
static uint32_t shift_reg(int type, uint32_t v, uint32_t amount, int *carry) {
    amount &= 0xFF;
    if (!amount) return v;
    switch (type) {
    case 0:
        if (amount < 32) {
            *carry = (v >> (32 - amount)) & 1;
            return v << amount;
        }
        *carry = amount == 32 ? (int)(v & 1) : 0;
        return 0;
    case 1:
        if (amount < 32) {
            *carry = (v >> (amount - 1)) & 1;
            return v >> amount;
        }
        *carry = amount == 32 ? (int)(v >> 31) : 0;
        return 0;
    case 2:
        if (amount < 32) {
            *carry = (v >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)v >> amount);
        }
        *carry = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default:
        amount &= 31;
        if (!amount) {
            *carry = v >> 31;
            return v;
        }
        *carry = (v >> (amount - 1)) & 1;
        return ror32(v, amount);
    }
}

/* ---- ARM: data processing ---- */

static void arm_dataproc(NdsArm *c, uint32_t op) {
    int opc = (op >> 21) & 15, s = (op >> 20) & 1;
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    int carry = carry_flag(c), flags, logical = 0, write = 1;
    uint32_t a = c->r[rn], b, res;

    if (op & (1u << 25)) {
        unsigned rot = (op >> 7) & 30;
        b = ror32(op & 0xFF, rot);
        if (rot) carry = b >> 31;
    } else if (op & 0x10) {
        /* Shift by register: the extra cycle makes r15 read 4 further on. */
        int rm = op & 15;
        uint32_t mv = c->r[rm] + (rm == 15 ? 4 : 0);
        if (rn == 15) a += 4;
        c->cycles++;
        b = shift_reg((op >> 5) & 3, mv, c->r[(op >> 8) & 15], &carry);
    } else {
        b = shift_imm((op >> 5) & 3, c->r[op & 15], (op >> 7) & 31, &carry);
    }

    /* With S and rd = r15 the flags come from SPSR instead (below). */
    flags = s && (rd != 15 || (opc >= 8 && opc <= 11));
    switch (opc) {
    case 0x0: res = a & b; logical = 1; break;
    case 0x1: res = a ^ b; logical = 1; break;
    case 0x2: res = alu_sub(c, a, b, 1, flags); break;
    case 0x3: res = alu_sub(c, b, a, 1, flags); break;
    case 0x4: res = alu_add(c, a, b, 0, flags); break;
    case 0x5: res = alu_add(c, a, b, (uint32_t)carry_flag(c), flags); break;
    case 0x6: res = alu_sub(c, a, b, (uint32_t)carry_flag(c), flags); break;
    case 0x7: res = alu_sub(c, b, a, (uint32_t)carry_flag(c), flags); break;
    case 0x8: res = a & b; logical = 1; write = 0; break;
    case 0x9: res = a ^ b; logical = 1; write = 0; break;
    case 0xA: res = alu_sub(c, a, b, 1, flags); write = 0; break;
    case 0xB: res = alu_add(c, a, b, 0, flags); write = 0; break;
    case 0xC: res = a | b; logical = 1; break;
    case 0xD: res = b; logical = 1; break;
    case 0xE: res = a & ~b; logical = 1; break;
    default: res = ~b; logical = 1; break;
    }
    if (logical && flags) set_nzc(c, res, carry);
    if (!write) return;
    if (rd == 15) {
        /* Data processing never interworks; S restores CPSR from SPSR. */
        if (s && bank_of(c->cpsr)) nds_arm_set_cpsr(c, c->spsr);
        nds_arm_jump(c, res);
    } else {
        c->r[rd] = res;
    }
}

/* ---- ARM: PSR transfer ---- */

static void arm_mrs(NdsArm *c, uint32_t op) {
    int rd = (op >> 12) & 15;
    c->r[rd] = (op & (1u << 22)) ? (bank_of(c->cpsr) ? c->spsr : c->cpsr) : c->cpsr;
}

static void arm_msr(NdsArm *c, uint32_t op, uint32_t value) {
    uint32_t mask = 0;
    if (op & (1u << 16)) mask |= 0x000000FFu;
    if (op & (1u << 17)) mask |= 0x0000FF00u;
    if (op & (1u << 18)) mask |= 0x00FF0000u;
    if (op & (1u << 19)) mask |= 0xFF000000u;
    mask &= c->v5 ? 0xF80000FFu : 0xF00000FFu;
    if (op & (1u << 22)) {
        if (bank_of(c->cpsr)) c->spsr = (c->spsr & ~mask) | (value & mask);
    } else {
        if ((c->cpsr & 0x1F) == ARM_MODE_USR) mask &= 0xFF000000u;
        mask &= ~FLAG_T; /* MSR never changes the CPSR state bit */
        nds_arm_set_cpsr(c, (c->cpsr & ~mask) | (value & mask));
    }
}

/* ---- ARM: multiplies ---- */

static void arm_multiply(NdsArm *c, uint32_t op) {
    int rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    uint32_t r = c->r[rm] * c->r[rs];
    if (op & (1u << 21)) {
        r += c->r[rn];
        c->cycles++;
    }
    c->cycles += 2;
    c->r[rd] = r;
    if (op & (1u << 20)) set_nz(c, r); /* C is left alone */
}

static void arm_multiply_long(NdsArm *c, uint32_t op) {
    int hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    uint64_t r;
    if (op & (1u << 22))
        r = (uint64_t)((int64_t)(int32_t)c->r[rm] * (int32_t)c->r[rs]);
    else
        r = (uint64_t)c->r[rm] * c->r[rs];
    if (op & (1u << 21)) r += ((uint64_t)c->r[hi] << 32) | c->r[lo];
    c->cycles += 3;
    c->r[lo] = (uint32_t)r;
    c->r[hi] = (uint32_t)(r >> 32);
    if (op & (1u << 20))
        c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z)) | ((uint32_t)(r >> 32) & FLAG_N) | (r ? 0 : FLAG_Z);
}

/* ARMv5TE signed halfword multiplies. A no-op on the ARM7. */
static void arm_dsp_multiply(NdsArm *c, uint32_t op) {
    int rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    int x = (op >> 5) & 1, y = (op >> 6) & 1;
    int32_t mhalf = (int16_t)(x ? c->r[rm] >> 16 : c->r[rm]);
    int32_t shalf = (int16_t)(y ? c->r[rs] >> 16 : c->r[rs]);
    if (!c->v5) return;
    c->cycles++;
    switch ((op >> 21) & 3) {
    case 0: { /* SMLAxy */
        uint32_t p = (uint32_t)(mhalf * shalf), acc = c->r[rn], r = p + acc;
        if (~(p ^ acc) & (p ^ r) & 0x80000000u) c->cpsr |= FLAG_Q;
        c->r[rd] = r;
        break;
    }
    case 1: { /* SMLAWy / SMULWy: 32x16, top 32 bits of the 48-bit product */
        uint32_t p = (uint32_t)(((int64_t)(int32_t)c->r[rm] * shalf) >> 16);
        if (x) {
            c->r[rd] = p;
        } else {
            uint32_t acc = c->r[rn], r = p + acc;
            if (~(p ^ acc) & (p ^ r) & 0x80000000u) c->cpsr |= FLAG_Q;
            c->r[rd] = r;
        }
        break;
    }
    case 2: { /* SMLALxy: rd is RdHi, rn RdLo */
        uint64_t acc = ((uint64_t)c->r[rd] << 32) | c->r[rn];
        acc += (uint64_t)(int64_t)(mhalf * shalf);
        c->r[rn] = (uint32_t)acc;
        c->r[rd] = (uint32_t)(acc >> 32);
        break;
    }
    default: /* SMULxy */
        c->r[rd] = (uint32_t)(mhalf * shalf);
        break;
    }
}

static uint32_t saturate(NdsArm *c, int64_t v) {
    if (v > 0x7FFFFFFF) {
        c->cpsr |= FLAG_Q;
        return 0x7FFFFFFFu;
    }
    if (v < -0x7FFFFFFF - 1) {
        c->cpsr |= FLAG_Q;
        return 0x80000000u;
    }
    return (uint32_t)v;
}

/* QADD, QSUB, QDADD, QDSUB (ARMv5TE). */
static void arm_saturating(NdsArm *c, uint32_t op) {
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    int64_t a = (int32_t)c->r[rm], b = (int32_t)c->r[rn];
    if (op & (1u << 22)) b = (int32_t)saturate(c, b * 2);
    c->r[rd] = saturate(c, (op & (1u << 21)) ? a - b : a + b);
}

/* ---- ARM: loads and stores ---- */

static void load_pc(NdsArm *c, uint32_t value) {
    /* ARMv5 loads into r15 interwork on bit 0; ARMv4 stays in ARM state. */
    if (c->v5) nds_arm_jump_bx(c, value);
    else nds_arm_jump(c, value);
}

static void arm_single(NdsArm *c, uint32_t op) {
    int p = (op >> 24) & 1, u = (op >> 23) & 1, byte = (op >> 22) & 1, w = (op >> 21) & 1;
    int load = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t off, base = c->r[rn], moved, addr;
    if (op & (1u << 25)) {
        int carry = carry_flag(c);
        off = shift_imm((op >> 5) & 3, c->r[op & 15], (op >> 7) & 31, &carry);
    } else {
        off = op & 0xFFF;
    }
    moved = u ? base + off : base - off;
    addr = p ? moved : base;
    if (load) {
        uint32_t v = byte ? ld8(c, addr) : ror32(ld32(c, addr & ~3u), (addr & 3) * 8);
        if ((!p || w) && rn != 15) c->r[rn] = moved; /* the loaded value wins over writeback */
        if (rd == 15) load_pc(c, v);
        else c->r[rd] = v;
    } else {
        uint32_t v = c->r[rd] + (rd == 15 ? 4 : 0);
        if (byte) st8(c, addr, v);
        else st32(c, addr & ~3u, v);
        if ((!p || w) && rn != 15) c->r[rn] = moved;
    }
}

static void arm_halfword(NdsArm *c, uint32_t op) {
    int p = (op >> 24) & 1, u = (op >> 23) & 1, w = (op >> 21) & 1, load = (op >> 20) & 1;
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
    uint32_t off = (op & (1u << 22)) ? (((op >> 4) & 0xF0) | (op & 0xF)) : c->r[op & 15];
    uint32_t base = c->r[rn], moved = u ? base + off : base - off, addr = p ? moved : base;
    int wb = (!p || w) && rn != 15;
    if (load) {
        uint32_t v;
        if (sh == 1) {
            v = ld16(c, addr & ~1u);
            if ((addr & 1) && !c->v5) v = ror32(v, 8); /* ARM7: misaligned LDRH rotates */
        } else if (sh == 2) {
            v = (uint32_t)(int8_t)ld8(c, addr);
        } else if ((addr & 1) && !c->v5) {
            v = (uint32_t)(int8_t)ld8(c, addr); /* ARM7: misaligned LDRSH reads a byte */
        } else {
            v = (uint32_t)(int16_t)ld16(c, addr & ~1u);
        }
        if (wb) c->r[rn] = moved;
        if (rd == 15) nds_arm_jump(c, v);
        else c->r[rd] = v;
        return;
    }
    if (sh == 1) {
        st16(c, addr & ~1u, c->r[rd] + (rd == 15 ? 4 : 0));
        if (wb) c->r[rn] = moved;
        return;
    }
    /* LDRD and STRD (ARMv5TE); the ARM7 ignores them. */
    if (!c->v5) return;
    rd &= ~1;
    if (sh == 2) {
        uint32_t lo = ld32(c, addr & ~3u), hi = ld32(c, (addr + 4) & ~3u);
        if (wb) c->r[rn] = moved;
        c->r[rd] = lo;
        if (rd + 1 == 15) nds_arm_jump(c, hi);
        else c->r[rd + 1] = hi;
    } else {
        st32(c, addr & ~3u, c->r[rd]);
        st32(c, (addr + 4) & ~3u, c->r[rd + 1] + (rd + 1 == 15 ? 4 : 0));
        if (wb) c->r[rn] = moved;
    }
}

static void arm_swap(NdsArm *c, uint32_t op) {
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    uint32_t addr = c->r[rn], src = c->r[rm], v;
    if (op & (1u << 22)) {
        v = ld8(c, addr);
        st8(c, addr, src);
    } else {
        v = ror32(ld32(c, addr & ~3u), (addr & 3) * 8);
        st32(c, addr & ~3u, src);
    }
    c->r[rd] = v;
}

/* LDM/STM and Thumb PUSH/POP/LDMIA/STMIA (GBATEK "ARM Opcodes: Memory: Block
 * Data Transfer", with rockwrestler's ARMv5 writeback cases). */
static void block_transfer(NdsArm *c, int rn, uint32_t list, int pre, int up, int wb, int load,
                           int s) {
    uint32_t base = c->r[rn], size, start, final, addr;
    int count = popcount16(list), user;
    if (!list) {
        /* Empty list: the base moves by 0x40; the ARM7 still transfers r15. */
        size = 0x40;
        if (!c->v5) list = 0x8000;
    } else {
        size = (uint32_t)count * 4;
    }
    if (up) {
        start = base + (pre ? 4 : 0);
        final = base + size;
    } else {
        final = base - size;
        start = final + (pre ? 0 : 4);
    }
    user = s && (!load || !(list & 0x8000));
    if (wb && rn == 15) wb = 0;
    addr = start;
    if (load) {
        uint32_t pc_value = 0;
        for (int i = 0; i < 16; i++) {
            uint32_t v;
            if (!((list >> i) & 1)) continue;
            v = ld32(c, addr & ~3u);
            addr += 4;
            if (i == 15) pc_value = v;
            else if (user) set_user_reg(c, i, v);
            else c->r[i] = v;
        }
        if (wb) {
            if (!((list >> rn) & 1)) c->r[rn] = final;
            /* Base in the list: ARMv4 keeps the loaded value; ARMv5 writes back
             * unless the base is the last of several registers. */
            else if (c->v5 && (count == 1 || (list >> (rn + 1)) != 0)) c->r[rn] = final;
        }
        if (list & 0x8000) {
            if (s && bank_of(c->cpsr)) {
                nds_arm_set_cpsr(c, c->spsr);
                nds_arm_jump(c, pc_value);
            } else {
                load_pc(c, pc_value);
            }
        }
    } else {
        int first = 1;
        for (int i = 0; i < 16; i++) {
            uint32_t v;
            if (!((list >> i) & 1)) continue;
            if (i == 15) v = c->r[15] + 4;
            /* ARMv4 stores the new base unless the base is first; ARMv5 the old. */
            else if (i == rn && wb && !c->v5 && !first) v = final;
            else v = user ? user_reg(c, i) : c->r[i];
            st32(c, addr & ~3u, v);
            addr += 4;
            first = 0;
        }
        if (wb) c->r[rn] = final;
    }
}

/* ---- ARM: branches, coprocessor, SWI ---- */

static void arm_branch(NdsArm *c, uint32_t op) {
    uint32_t offset = (uint32_t)(((int32_t)(op << 8)) >> 6);
    if (op & (1u << 24)) c->r[14] = c->r[15] - 4;
    nds_arm_jump(c, c->r[15] + offset);
}

static void arm_coprocessor(NdsArm *c, uint32_t op) {
    int rd = (op >> 12) & 15;
    if (!c->v5 || ((op >> 8) & 15) != 15 || !(op & 0x10)) {
        undefined(c);
        return;
    }
    if (op & (1u << 20)) {
        uint32_t v = nds_cp15_read(c->nds, (op >> 16) & 15, op & 15, (op >> 21) & 7, (op >> 5) & 7);
        if (rd == 15) c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (v & 0xF0000000u);
        else c->r[rd] = v;
    } else {
        nds_cp15_write(c->nds, (op >> 16) & 15, op & 15, (op >> 21) & 7, (op >> 5) & 7,
                       c->r[rd] + (rd == 15 ? 4 : 0));
    }
}

/* The opcode space with bits 27-23 = 00010 and bit 20 clear: PSR transfer, BX,
 * BLX, CLZ, saturating arithmetic, the DSP multiplies, BKPT. */
static void arm_misc(NdsArm *c, uint32_t op) {
    int op2 = (op >> 21) & 3;
    switch ((op >> 4) & 15) {
    case 0x0:
        if (op & (1u << 21)) arm_msr(c, op, c->r[op & 15]);
        else arm_mrs(c, op);
        return;
    case 0x1:
        if (op2 == 1) {
            nds_arm_jump_bx(c, c->r[op & 15]);
            return;
        }
        if (op2 == 3 && c->v5) {
            uint32_t v = c->r[op & 15], n = 0;
            while (n < 32 && !(v & 0x80000000u)) {
                v <<= 1;
                n++;
            }
            c->r[(op >> 12) & 15] = n;
            return;
        }
        break;
    case 0x3:
        if (op2 == 1 && c->v5) {
            uint32_t target = c->r[op & 15];
            c->r[14] = c->r[15] - 4;
            nds_arm_jump_bx(c, target);
            return;
        }
        break;
    case 0x5:
        if (c->v5) {
            arm_saturating(c, op);
            return;
        }
        break;
    case 0x8: case 0xA: case 0xC: case 0xE:
        arm_dsp_multiply(c, op);
        return;
    default:
        break;
    }
    undefined(c);
}

static void arm_execute(NdsArm *c, uint32_t op) {
    uint32_t cond = op >> 28;
    if (cond != 0xE && !cond_passed(c->cpsr, cond)) {
        if (cond != 0xF || !c->v5) return;
        /* The ARMv5 unconditional space: BLX immediate and PLD. */
        if ((op & 0x0E000000u) == 0x0A000000u) {
            uint32_t offset = (uint32_t)(((int32_t)(op << 8)) >> 6) | ((op >> 23) & 2);
            c->r[14] = c->r[15] - 4;
            c->cpsr |= FLAG_T;
            nds_arm_jump(c, c->r[15] + offset);
        } else if ((op & 0x0D70F000u) != 0x0550F000u) {
            undefined(c);
        }
        return;
    }
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x90) == 0x90) {
            if (op & 0x60) arm_halfword(c, op);
            else if ((op & 0x0FB00FF0u) == 0x01000090u) arm_swap(c, op);
            else if ((op & 0x0F800000u) == 0x00800000u) arm_multiply_long(c, op);
            else if ((op & 0x0FC00000u) == 0x00000000u) arm_multiply(c, op);
            else undefined(c);
        } else if ((op & 0x01900000u) == 0x01000000u) {
            arm_misc(c, op);
        } else {
            arm_dataproc(c, op);
        }
        return;
    case 1:
        if ((op & 0x01900000u) == 0x01000000u) {
            if (op & (1u << 21)) arm_msr(c, op, ror32(op & 0xFF, (op >> 7) & 30));
            else undefined(c);
        } else {
            arm_dataproc(c, op);
        }
        return;
    case 2:
        arm_single(c, op);
        return;
    case 3:
        if (op & 0x10) undefined(c);
        else arm_single(c, op);
        return;
    case 4:
        block_transfer(c, (op >> 16) & 15, op & 0xFFFF, (op >> 24) & 1, (op >> 23) & 1,
                       (op >> 21) & 1, (op >> 20) & 1, (op >> 22) & 1);
        return;
    case 5:
        arm_branch(c, op);
        return;
    case 6:
        undefined(c); /* LDC/STC: no coprocessor takes them */
        return;
    default:
        if (op & (1u << 24)) nds_bios_swi(c, (op >> 16) & 0xFF);
        else arm_coprocessor(c, op);
        return;
    }
}

/* ---- Thumb ---- */

static void thumb_alu(NdsArm *c, uint16_t op) {
    int rd = op & 7;
    uint32_t a = c->r[rd], b = c->r[(op >> 3) & 7], r;
    int carry = carry_flag(c);
    switch ((op >> 6) & 15) {
    case 0x0: r = a & b; set_nz(c, r); c->r[rd] = r; break;
    case 0x1: r = a ^ b; set_nz(c, r); c->r[rd] = r; break;
    case 0x2: c->cycles++; r = shift_reg(0, a, b, &carry); set_nzc(c, r, carry); c->r[rd] = r; break;
    case 0x3: c->cycles++; r = shift_reg(1, a, b, &carry); set_nzc(c, r, carry); c->r[rd] = r; break;
    case 0x4: c->cycles++; r = shift_reg(2, a, b, &carry); set_nzc(c, r, carry); c->r[rd] = r; break;
    case 0x5: c->r[rd] = alu_add(c, a, b, (uint32_t)carry, 1); break;
    case 0x6: c->r[rd] = alu_sub(c, a, b, (uint32_t)carry, 1); break;
    case 0x7: c->cycles++; r = shift_reg(3, a, b, &carry); set_nzc(c, r, carry); c->r[rd] = r; break;
    case 0x8: set_nz(c, a & b); break;
    case 0x9: c->r[rd] = alu_sub(c, 0, b, 1, 1); break;
    case 0xA: alu_sub(c, a, b, 1, 1); break;
    case 0xB: alu_add(c, a, b, 0, 1); break;
    case 0xC: r = a | b; set_nz(c, r); c->r[rd] = r; break;
    case 0xD: c->cycles += 2; r = a * b; set_nz(c, r); c->r[rd] = r; break;
    case 0xE: r = a & ~b; set_nz(c, r); c->r[rd] = r; break;
    default: r = ~b; set_nz(c, r); c->r[rd] = r; break;
    }
}

static void thumb_hireg(NdsArm *c, uint16_t op) {
    int rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
    uint32_t v = c->r[rs];
    switch ((op >> 8) & 3) {
    case 0: {
        uint32_t r = c->r[rd] + v;
        if (rd == 15) nds_arm_jump(c, r);
        else c->r[rd] = r;
        break;
    }
    case 1:
        alu_sub(c, c->r[rd], v, 1, 1);
        break;
    case 2:
        if (rd == 15) nds_arm_jump(c, v);
        else c->r[rd] = v;
        break;
    default:
        if (op & 0x80) {
            if (!c->v5) {
                undefined(c);
                return;
            }
            c->r[14] = (c->r[15] - 2) | 1;
        }
        nds_arm_jump_bx(c, v);
        break;
    }
}

static void thumb_load_store_reg(NdsArm *c, uint16_t op) {
    int rd = op & 7;
    uint32_t addr = c->r[(op >> 3) & 7] + c->r[(op >> 6) & 7], v;
    switch ((op >> 9) & 7) {
    case 0: st32(c, addr & ~3u, c->r[rd]); break;
    case 1: st16(c, addr & ~1u, c->r[rd]); break;
    case 2: st8(c, addr, c->r[rd]); break;
    case 3: c->r[rd] = (uint32_t)(int8_t)ld8(c, addr); break;
    case 4: c->r[rd] = ror32(ld32(c, addr & ~3u), (addr & 3) * 8); break;
    case 5:
        v = ld16(c, addr & ~1u);
        if ((addr & 1) && !c->v5) v = ror32(v, 8);
        c->r[rd] = v;
        break;
    case 6: c->r[rd] = ld8(c, addr); break;
    default:
        if ((addr & 1) && !c->v5) c->r[rd] = (uint32_t)(int8_t)ld8(c, addr);
        else c->r[rd] = (uint32_t)(int16_t)ld16(c, addr & ~1u);
        break;
    }
}

static void thumb_execute(NdsArm *c, uint16_t op) {
    int rd;
    uint32_t addr, v;
    switch (op >> 13) {
    case 0:
        if (((op >> 11) & 3) != 3) {
            int carry = carry_flag(c);
            uint32_t r = shift_imm((op >> 11) & 3, c->r[(op >> 3) & 7], (op >> 6) & 31, &carry);
            set_nzc(c, r, carry);
            c->r[op & 7] = r;
        } else {
            uint32_t a = c->r[(op >> 3) & 7], x = (op >> 6) & 7;
            uint32_t b = (op & 0x400) ? x : c->r[x];
            c->r[op & 7] = (op & 0x200) ? alu_sub(c, a, b, 1, 1) : alu_add(c, a, b, 0, 1);
        }
        return;
    case 1:
        rd = (op >> 8) & 7;
        v = op & 0xFF;
        switch ((op >> 11) & 3) {
        case 0: c->r[rd] = v; set_nz(c, v); break;
        case 1: alu_sub(c, c->r[rd], v, 1, 1); break;
        case 2: c->r[rd] = alu_add(c, c->r[rd], v, 0, 1); break;
        default: c->r[rd] = alu_sub(c, c->r[rd], v, 1, 1); break;
        }
        return;
    case 2:
        if (op & 0x1000) thumb_load_store_reg(c, op);
        else if (op & 0x0800) c->r[(op >> 8) & 7] = ld32(c, ((c->r[15] & ~2u) + (op & 0xFFu) * 4));
        else if (op & 0x0400) thumb_hireg(c, op);
        else thumb_alu(c, op);
        return;
    case 3: {
        uint32_t imm = (op >> 6) & 31;
        rd = op & 7;
        if (op & 0x1000) {
            addr = c->r[(op >> 3) & 7] + imm;
            if (op & 0x800) c->r[rd] = ld8(c, addr);
            else st8(c, addr, c->r[rd]);
        } else {
            addr = c->r[(op >> 3) & 7] + imm * 4;
            if (op & 0x800) c->r[rd] = ror32(ld32(c, addr & ~3u), (addr & 3) * 8);
            else st32(c, addr & ~3u, c->r[rd]);
        }
        return;
    }
    case 4:
        if (op & 0x1000) {
            rd = (op >> 8) & 7;
            addr = c->r[13] + (op & 0xFFu) * 4;
            if (op & 0x800) c->r[rd] = ror32(ld32(c, addr & ~3u), (addr & 3) * 8);
            else st32(c, addr & ~3u, c->r[rd]);
        } else {
            rd = op & 7;
            addr = c->r[(op >> 3) & 7] + ((op >> 6) & 31) * 2;
            if (op & 0x800) {
                v = ld16(c, addr & ~1u);
                if ((addr & 1) && !c->v5) v = ror32(v, 8);
                c->r[rd] = v;
            } else {
                st16(c, addr & ~1u, c->r[rd]);
            }
        }
        return;
    case 5:
        if (!(op & 0x1000)) {
            v = (op & 0x800) ? c->r[13] : (c->r[15] & ~2u);
            c->r[(op >> 8) & 7] = v + (op & 0xFFu) * 4;
        } else if ((op & 0x0F00) == 0x0000) {
            uint32_t off = (op & 0x7Fu) * 4;
            c->r[13] = (op & 0x80) ? c->r[13] - off : c->r[13] + off;
        } else if ((op & 0x0600) == 0x0400) {
            uint32_t list = op & 0xFF;
            if (op & 0x800) {
                if (op & 0x100) list |= 0x8000;
                block_transfer(c, 13, list, 0, 1, 1, 1, 0);
            } else {
                if (op & 0x100) list |= 0x4000;
                block_transfer(c, 13, list, 1, 0, 1, 0, 0);
            }
        } else {
            undefined(c); /* BKPT and unused encodings */
        }
        return;
    case 6:
        if (!(op & 0x1000)) {
            block_transfer(c, (op >> 8) & 7, op & 0xFF, 0, 1, 1, (op >> 11) & 1, 0);
        } else {
            uint32_t cond = (op >> 8) & 15;
            if (cond == 0xF) nds_bios_swi(c, op & 0xFF);
            else if (cond == 0xE) undefined(c);
            else if (cond_passed(c->cpsr, cond)) nds_arm_jump(c, c->r[15] + (uint32_t)((int32_t)(int8_t)op * 2));
        }
        return;
    default: {
        uint32_t off11 = op & 0x7FFu;
        switch ((op >> 11) & 3) {
        case 0:
            nds_arm_jump(c, c->r[15] + (uint32_t)(((int32_t)(off11 << 21)) >> 20));
            break;
        case 1: /* BLX suffix (ARMv5): to ARM */
            if (!c->v5 || (op & 1)) {
                undefined(c);
                break;
            }
            v = (c->r[14] + off11 * 2) & ~3u;
            c->r[14] = (c->r[15] - 2) | 1;
            c->cpsr &= ~FLAG_T;
            nds_arm_jump(c, v);
            break;
        case 2:
            c->r[14] = c->r[15] + (uint32_t)(((int32_t)(off11 << 21)) >> 9);
            break;
        default:
            v = c->r[14] + off11 * 2;
            c->r[14] = (c->r[15] - 2) | 1;
            nds_arm_jump(c, v);
            break;
        }
        return;
    }
    }
}

/* ---- The step loop ---- */

void nds_arm_step(NdsArm *c) {
    c->branched = 0;
    c->cycles++;
    if (c->cpsr & FLAG_T) {
        uint16_t op = nds_bus_fetch16(c, c->r[15] - 4);
        thumb_execute(c, op);
        if (!c->branched) c->r[15] += 2;
    } else {
        uint32_t op = nds_bus_fetch32(c, c->r[15] - 8);
        arm_execute(c, op);
        if (!c->branched) c->r[15] += 4;
    }
}

void nds_arm_run(NdsArm *c, uint64_t target) {
    while (c->cycles < target) {
        if (c->halted) {
            c->cycles = target;
            return;
        }
        if (c->irq_line && !(c->cpsr & ARM_FLAG_I))
            nds_arm_exception(c, ARM_MODE_IRQ, 0x18, nds_arm_pc(c) + 4);
        nds_arm_step(c);
    }
}
