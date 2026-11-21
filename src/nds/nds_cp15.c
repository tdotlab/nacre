/* The ARM9's system control coprocessor (GBATEK, "ARM CP15 System Control
 * Coprocessor"). Caches and the protection unit are stored but not modelled;
 * what matters to software is the ID registers, the control register, the TCM
 * placement and the halt operation. */

#include "nds_internal.h"

#include <string.h>

#define CONTROL_RW 0x000FF085u    /* bits 0, 2, 7 and 12-19 */
#define CONTROL_FIXED 0x00000078u /* bits 3-6 read as 1 */

/* Virtual size of a TCM region: 512 << N, at least 4 KiB. Returns the mask of
 * address bits that select the region. */
static uint32_t tcm_mask(uint32_t reg) {
    unsigned n = (reg >> 1) & 0x1F;
    uint64_t size = 512ull << n;
    if (size < 0x1000) size = 0x1000;
    if (size >= 0x100000000ull) return 0;
    return ~(uint32_t)(size - 1);
}

static void update_tcm(Nds *nds) {
    NdsCp15 *cp = &nds->cp15;
    uint32_t control = cp->control;
    cp->itcm_write = (control >> 18) & 1;
    cp->itcm_read = cp->itcm_write && !((control >> 19) & 1);
    cp->dtcm_write = (control >> 16) & 1;
    cp->dtcm_read = cp->dtcm_write && !((control >> 17) & 1);
    cp->itcm_mask = tcm_mask(cp->itcm_reg); /* the ITCM base is fixed at 0 */
    cp->dtcm_mask = tcm_mask(cp->dtcm_reg);
    cp->dtcm_base = cp->dtcm_reg & cp->dtcm_mask & 0xFFFFF000u;
    nds->arm9.vector_base = (control & (1u << 13)) ? 0xFFFF0000u : 0;
}

void nds_cp15_reset(Nds *nds) {
    memset(&nds->cp15, 0, sizeof nds->cp15);
    nds->cp15.control = CONTROL_FIXED | (1u << 13); /* high vectors at power-on */
    update_tcm(nds);
}

uint32_t nds_cp15_read(Nds *nds, int crn, int crm, int op1, int op2) {
    NdsCp15 *cp = &nds->cp15;
    if (op1) return 0;
    switch (crn) {
    case 0:
        if (crm) return 0;
        if (op2 == 1) return 0x0F0D2112u; /* cache type */
        if (op2 == 2) return 0x00140180u; /* TCM size */
        return 0x41059461u;               /* main ID: ARM946E-S */
    case 1:
        return crm == 0 && op2 == 0 ? cp->control : 0;
    case 6:
        return cp->regs[6][crm][0]; /* protection regions: one unified set */
    case 7:
        return 0;
    case 9:
        if (crm == 1) return op2 == 0 ? cp->dtcm_reg : op2 == 1 ? cp->itcm_reg : 0;
        return cp->regs[9][crm][op2];
    default:
        return cp->regs[crn][crm][op2];
    }
}

void nds_cp15_write(Nds *nds, int crn, int crm, int op1, int op2, uint32_t value) {
    NdsCp15 *cp = &nds->cp15;
    if (op1) return;
    switch (crn) {
    case 0:
        return;
    case 1:
        if (crm == 0 && op2 == 0) {
            cp->control = (cp->control & ~CONTROL_RW) | (value & CONTROL_RW) | CONTROL_FIXED;
            update_tcm(nds);
        }
        return;
    case 6:
        cp->regs[6][crm][0] = value;
        return;
    case 7:
        /* Cache maintenance is a no-op; two encodings halt until an interrupt. */
        if ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2)) {
            nds->arm9.halted = !(nds->ie[0] & nds->irf[0]);
        }
        return;
    case 9:
        if (crm == 1) {
            if (op2 == 0) cp->dtcm_reg = value & 0xFFFFF03Eu;
            else if (op2 == 1) cp->itcm_reg = value & 0xFFFFF03Eu;
            update_tcm(nds);
            return;
        }
        cp->regs[9][crm][op2] = value;
        return;
    default:
        cp->regs[crn][crm][op2] = value;
        return;
    }
}
