#ifndef NACRE_NDS_ARM_H
#define NACRE_NDS_ARM_H

/* One ARM/Thumb interpreter for both DS CPUs: the ARM946E-S (ARMv5TE) and the
 * ARM7TDMI (ARMv4T). Written from GBATEK's ARM chapters. Not part of the API. */

#include <stdint.h>

struct Nds;

enum {
    ARM_MODE_USR = 0x10,
    ARM_MODE_FIQ = 0x11,
    ARM_MODE_IRQ = 0x12,
    ARM_MODE_SVC = 0x13,
    ARM_MODE_ABT = 0x17,
    ARM_MODE_UND = 0x1B,
    ARM_MODE_SYS = 0x1F,
};

#define ARM_FLAG_N 0x80000000u
#define ARM_FLAG_Z 0x40000000u
#define ARM_FLAG_C 0x20000000u
#define ARM_FLAG_V 0x10000000u
#define ARM_FLAG_Q 0x08000000u
#define ARM_FLAG_I 0x80u
#define ARM_FLAG_F 0x40u
#define ARM_FLAG_T 0x20u

typedef struct NdsArm {
    /* r[15] reads as the executing instruction's address plus 8 (ARM) or 4
     * (Thumb), as the pipeline makes it on hardware. */
    uint32_t r[16];
    uint32_t cpsr, spsr;
    /* Banked r13, r14 and SPSR by bank: usr/sys, fiq, irq, svc, abt, und. */
    uint32_t bank_r13[6], bank_r14[6], bank_spsr[6];
    uint32_t usr_r8[5], fiq_r8[5]; /* r8-r12 outside and inside FIQ mode */

    int v5;               /* 1 for the ARM9 */
    int halted;           /* waiting for (IE & IF) != 0 */
    int irq_line;         /* IME && (IE & IF): the IRQ input, taken when CPSR.I is clear */
    int branched;         /* set by any write to r15 during an instruction */
    uint32_t vector_base; /* 0 or 0xFFFF0000 */
    uint64_t cycles;      /* in this CPU's own clock */
    uint64_t undefined_count; /* undefined instructions taken, for diagnostics */
    uint32_t last_undefined;  /* address of the latest one */

    /* BIOS IntrWait in progress: the SWI re-executes until a flag arrives. */
    int intr_waiting;

    struct Nds *nds;
} NdsArm;

void nds_arm_reset(NdsArm *cpu, struct Nds *nds, int v5);
/* Sets CPSR, swapping banked registers when the mode changes. */
void nds_arm_set_cpsr(NdsArm *cpu, uint32_t value);
/* Jumps to `address` as a branch, keeping the current state (ARM/Thumb). */
void nds_arm_jump(NdsArm *cpu, uint32_t address);
/* Jumps and selects Thumb by bit 0, like BX. */
void nds_arm_jump_bx(NdsArm *cpu, uint32_t address);
/* Address of the instruction that would execute next. */
uint32_t nds_arm_pc(const NdsArm *cpu);
/* Runs until the CPU's cycle counter reaches `target` or it halts. */
void nds_arm_run(NdsArm *cpu, uint64_t target);
/* Executes one instruction, ignoring interrupts and halt (for BIOS guest calls). */
void nds_arm_step(NdsArm *cpu);
/* Enters an exception: mode, vector offset, and the return address for lr. */
void nds_arm_exception(NdsArm *cpu, uint32_t mode, uint32_t vector, uint32_t lr);
/* Banked register access for boot code: r13/r14 of a given mode. */
void nds_arm_set_banked_sp(NdsArm *cpu, uint32_t mode, uint32_t sp);
void nds_arm_set_banked_lr(NdsArm *cpu, uint32_t mode, uint32_t lr);

#endif
