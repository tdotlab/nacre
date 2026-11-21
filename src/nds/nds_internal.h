#ifndef NACRE_NDS_INTERNAL_H
#define NACRE_NDS_INTERNAL_H

/* Machine state shared by the DS core's source files. Not part of the API. */

#include "nds.h"
#include "nds_arm.h"

#define NDS_AUDIO_RING 16384u /* stereo frames, about a third of a second */
#define NDS_MAX_ROM (512u << 20)
#define NDS_HEADER_SIZE 0x200u

/* Main RAM and the ARM7's private RAM, where direct boot may place binaries. */
#define NDS_MAIN_RAM_BASE 0x02000000u
#define NDS_MAIN_RAM_SIZE (4u << 20)
#define NDS_ARM7_WRAM_BASE 0x037F8000u /* shared WRAM mapped to the ARM7, then its own 64 KiB */
#define NDS_ARM7_WRAM_END 0x03810000u

#define NDS_SHARED_WRAM_SIZE 0x8000u
#define NDS_ARM7_WRAM_SIZE 0x10000u
#define NDS_VRAM_SIZE 0xA4000u /* banks A-I, 656 KiB */
#define NDS_ITCM_SIZE 0x8000u
#define NDS_DTCM_SIZE 0x4000u
#define NDS_BIOS9_SIZE 0x1000u
#define NDS_BIOS7_SIZE 0x4000u
#define NDS_IO_SIZE 0x2000u

/* Timing within a frame, in ARM7 cycles (GBATEK, "DS LCD Timings"). */
#define NDS_LINE_CYCLES 2130u
#define NDS_HBLANK_CYCLE 1536u
#define NDS_LINES 263u

/* IRQ bits shared by IE and IF. */
#define NDS_IRQ_VBLANK (1u << 0)
#define NDS_IRQ_HBLANK (1u << 1)
#define NDS_IRQ_VCOUNT (1u << 2)
#define NDS_IRQ_IPCSYNC (1u << 16)
#define NDS_IRQ_FIFO_EMPTY (1u << 17)
#define NDS_IRQ_FIFO_RECV (1u << 18)

/* Where the BIOS's guest-call trampoline returns to (an instruction that loops). */
#define NDS_BIOS9_CALL_RETURN 0xFFFF0100u
#define NDS_BIOS7_CALL_RETURN 0x00000100u

/* The parts of the cartridge header the core uses (GBATEK, "DS Cartridge Header"). */
typedef struct {
    char title[13];
    char game_code[5];
    char maker[3];
    uint8_t unit_code;
    uint8_t capacity;       /* chip size is 128 KiB << capacity */
    uint32_t arm9_offset, arm9_entry, arm9_address, arm9_size;
    uint32_t arm7_offset, arm7_entry, arm7_address, arm7_size;
    uint32_t banner_offset;
    uint32_t rom_size;      /* total used ROM size */
} NdsHeader;

/* One VRAM page (16 KiB of CPU address space) and the banks mapped there. Most
 * pages have one bank; overlapping banks read ORed and write to all. */
typedef struct {
    uint8_t count;
    uint8_t *bank[4];
} NdsVramPage;

/* The VRAM regions the ARM9 sees, in 16 KiB pages. */
enum { NDS_VR_ABG, NDS_VR_BBG, NDS_VR_AOBJ, NDS_VR_BOBJ, NDS_VR_LCDC, NDS_VR_COUNT };

/* ARM946E-S system control coprocessor (GBATEK, "ARM CP15 System Control
 * Coprocessor"). Registers without behavior are stored and read back. */
typedef struct {
    uint32_t control;
    uint32_t dtcm_reg, itcm_reg;
    uint32_t regs[16][16][8]; /* by CRn, CRm, opcode 2 */
    /* Derived TCM placement. A TCM "reads" when enabled and not in load mode. */
    int itcm_read, itcm_write, dtcm_read, dtcm_write;
    uint32_t itcm_mask;      /* address bits that must be zero for an ITCM hit */
    uint32_t dtcm_mask, dtcm_base;
} NdsCp15;

/* One direction of the IPC FIFO: words sent by one CPU to the other. */
typedef struct {
    uint32_t data[16];
    int head, count;
} NdsFifo;

struct Nds {
    uint8_t *rom;
    size_t rom_size;
    NdsHeader header;
    char title[128];

    uint64_t cycles;        /* ARM7 cycles since power-on */
    uint32_t frame_cycle;   /* position within the current frame */
    uint32_t frame_count;
    uint32_t line;          /* VCOUNT, 0..262 */
    uint32_t line_cycle;    /* position within the line */
    uint64_t pace_target;   /* cycle count advance_ns is running toward */
    uint64_t pace_fraction; /* its remainder, in nanosecond-cycles */
    uint64_t sample_fraction; /* fractional audio frame, in units of 1/NDS_ARM7_HZ */

    uint16_t buttons;       /* bit per NdsButton, 1 = held */
    int touch_x, touch_y, touching;

    /* The two CPUs. Index 0 is the ARM9, 1 the ARM7, everywhere below. */
    NdsArm arm9, arm7;
    NdsCp15 cp15;

    uint8_t main_ram[NDS_MAIN_RAM_SIZE];
    uint8_t shared_wram[NDS_SHARED_WRAM_SIZE];
    uint8_t arm7_wram[NDS_ARM7_WRAM_SIZE];
    uint8_t vram[NDS_VRAM_SIZE];
    uint8_t palette[0x800];
    uint8_t oam[0x800];
    uint8_t itcm[NDS_ITCM_SIZE];
    uint8_t dtcm[NDS_DTCM_SIZE];
    uint8_t bios9[NDS_BIOS9_SIZE];
    uint8_t bios7[NDS_BIOS7_SIZE];
    /* Registers without special behavior are kept here, so writes read back. */
    uint8_t io9[NDS_IO_SIZE], io7[NDS_IO_SIZE];

    /* Shared WRAM split (WRAMCNT): offset and size each CPU sees. */
    uint8_t wramcnt;
    uint32_t wram9_offset, wram9_size, wram7_offset, wram7_size;

    /* VRAM banks and their mapping (VRAMCNT_A..I). */
    uint8_t vramcnt[9];
    NdsVramPage vmap9[NDS_VR_COUNT][64];
    NdsVramPage vmap7[16];

    /* Interrupts. */
    uint32_t ime[2], ie[2], irf[2];

    /* Display status, per CPU: the IRQ enables and VCOUNT compare (DISPSTAT). */
    uint16_t dispstat[2];
    uint32_t powcnt1;

    /* IPC. */
    uint16_t ipcsync[2];    /* output bits 8-11 and IRQ enable bit 14, per CPU */
    uint16_t fifocnt[2];    /* the IRQ enables, error and enable bits, per CPU */
    NdsFifo fifo[2];        /* fifo[i] holds words CPU i sent */
    uint32_t fifo_last[2];  /* last word CPU i received */

    uint8_t postflg[2];

    uint32_t framebuffer[NDS_FRAME_WIDTH * NDS_FRAME_HEIGHT];

    int16_t audio[NDS_AUDIO_RING * 2];
    size_t audio_read, audio_write; /* ring positions; equal when empty */
};

/* nds_cart.c */
int nds_parse_header(const uint8_t *rom, size_t length, NdsHeader *header, char *error,
                     size_t error_capacity);
void nds_banner_title(const uint8_t *rom, size_t length, const NdsHeader *header, char *out,
                      size_t capacity);

/* nds_mem.c: each CPU's bus. Addresses are aligned to the access size here. */
uint32_t nds9_read32(Nds *nds, uint32_t addr);
uint16_t nds9_read16(Nds *nds, uint32_t addr);
uint8_t nds9_read8(Nds *nds, uint32_t addr);
void nds9_write32(Nds *nds, uint32_t addr, uint32_t value);
void nds9_write16(Nds *nds, uint32_t addr, uint16_t value);
void nds9_write8(Nds *nds, uint32_t addr, uint8_t value);
uint32_t nds9_fetch32(Nds *nds, uint32_t addr);
uint16_t nds9_fetch16(Nds *nds, uint32_t addr);
uint32_t nds7_read32(Nds *nds, uint32_t addr);
uint16_t nds7_read16(Nds *nds, uint32_t addr);
uint8_t nds7_read8(Nds *nds, uint32_t addr);
void nds7_write32(Nds *nds, uint32_t addr, uint32_t value);
void nds7_write16(Nds *nds, uint32_t addr, uint16_t value);
void nds7_write8(Nds *nds, uint32_t addr, uint8_t value);
void nds_mem_reset(Nds *nds);
void nds_map_wram(Nds *nds);
void nds_map_vram(Nds *nds);

/* The bus of whichever CPU is executing. */
static inline uint32_t nds_bus_read32(NdsArm *c, uint32_t a) {
    return c->v5 ? nds9_read32(c->nds, a) : nds7_read32(c->nds, a);
}
static inline uint16_t nds_bus_read16(NdsArm *c, uint32_t a) {
    return c->v5 ? nds9_read16(c->nds, a) : nds7_read16(c->nds, a);
}
static inline uint8_t nds_bus_read8(NdsArm *c, uint32_t a) {
    return c->v5 ? nds9_read8(c->nds, a) : nds7_read8(c->nds, a);
}
static inline void nds_bus_write32(NdsArm *c, uint32_t a, uint32_t v) {
    if (c->v5) nds9_write32(c->nds, a, v); else nds7_write32(c->nds, a, v);
}
static inline void nds_bus_write16(NdsArm *c, uint32_t a, uint16_t v) {
    if (c->v5) nds9_write16(c->nds, a, v); else nds7_write16(c->nds, a, v);
}
static inline void nds_bus_write8(NdsArm *c, uint32_t a, uint8_t v) {
    if (c->v5) nds9_write8(c->nds, a, v); else nds7_write8(c->nds, a, v);
}
static inline uint32_t nds_bus_fetch32(NdsArm *c, uint32_t a) {
    return c->v5 ? nds9_fetch32(c->nds, a) : nds7_read32(c->nds, a);
}
static inline uint16_t nds_bus_fetch16(NdsArm *c, uint32_t a) {
    return c->v5 ? nds9_fetch16(c->nds, a) : nds7_read16(c->nds, a);
}

/* nds_io.c: registers at 0x04000000. `mask` selects the bytes a write touches. */
uint32_t nds_io_read(Nds *nds, int cpu, uint32_t addr);
void nds_io_write(Nds *nds, int cpu, uint32_t addr, uint32_t value, uint32_t mask);
void nds_io_reset(Nds *nds);
void nds_irq_update(Nds *nds, int cpu);
void nds_irq_raise(Nds *nds, int cpu, uint32_t bits);
uint32_t nds_dispstat_read(const Nds *nds, int cpu);

/* nds_cp15.c */
void nds_cp15_reset(Nds *nds);
uint32_t nds_cp15_read(Nds *nds, int crn, int crm, int op1, int op2);
void nds_cp15_write(Nds *nds, int crn, int crm, int op1, int op2, uint32_t value);

/* nds_bios.c: our own BIOS (vectors, IRQ trampolines, SWIs) and direct boot. */
void nds_bios_build(Nds *nds);
void nds_direct_boot(Nds *nds);
/* Runs SWI `number` for `cpu`. It may leave the CPU halted with r15 unchanged, so
 * the SWI runs again after an interrupt (IntrWait). */
void nds_bios_swi(NdsArm *cpu, uint32_t number);

/* nds_gpu2d.c */
void nds_render_line(Nds *nds, uint32_t line);

#endif
