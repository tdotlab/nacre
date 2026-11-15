#ifndef NACRE_GB_INTERNAL_H
#define NACRE_GB_INTERNAL_H

#include "gb.h"

#define GB_IRQ_VBLANK 0x01u
#define GB_IRQ_STAT 0x02u
#define GB_IRQ_TIMER 0x04u
#define GB_IRQ_SERIAL 0x08u
#define GB_IRQ_JOYPAD 0x10u

#define GB_SERIAL_CAPACITY 8192u
#define GB_AUDIO_CAPACITY 16384u /* stereo frames, power of two */

typedef enum { GB_MBC_NONE, GB_MBC1, GB_MBC2, GB_MBC3, GB_MBC5 } GbMbcKind;

typedef struct {
    uint8_t seconds, minutes, hours, days_low, days_high; /* days_high: bit0 day 8, bit6 halt, bit7 carry */
} GbRtcRegs;

typedef struct {
    uint8_t *rom;
    size_t rom_size;      /* power of two, >= 32 KiB */
    uint8_t *ram;
    size_t ram_size;
    GbMbcKind mbc;
    int has_battery;
    int has_rtc;
    int ram_enabled;
    uint16_t rom_bank;    /* switchable bank register (meaning depends on MBC) */
    uint8_t ram_bank;     /* RAM bank or MBC1 upper bits or MBC3 RTC select */
    uint8_t mode;         /* MBC1 banking mode */
    GbRtcRegs rtc;        /* live clock */
    GbRtcRegs latched;
    uint8_t latch_prev;
    uint32_t rtc_subsecond; /* T-cycles toward the next second */
    int64_t wall_clock;   /* last wall-clock time handed in by the frontend */
    int dirty;
    char title[17];
} GbCart;

typedef struct {
    uint8_t lcdc, stat, scy, scx, ly, lyc, bgp, obp0, obp1, wy, wx, dma;
    int dot;              /* 0..455 within the line */
    int mode;
    int mode3_end;
    int window_line;
    int window_triggered;
    int stat_line;
    uint32_t off_cycles;
    uint32_t frame_count;
    uint8_t back[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
    uint8_t front[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
} GbPpu;

typedef struct {
    int enabled;          /* DAC on */
    int active;           /* channel playing */
    int length;           /* length counter */
    int length_enabled;
    int timer;            /* T-cycles until the next waveform step */
    int position;
    int volume;
    int env_initial, env_up, env_period, env_timer;
    int duty;
    /* square 1 sweep */
    int sweep_period, sweep_negate, sweep_shift, sweep_timer, sweep_enabled, sweep_shadow;
    int sweep_negated_used;
    /* noise */
    uint16_t lfsr;
    int frequency;
} GbChannel;

typedef struct {
    uint8_t regs[0x30];   /* FF10..FF3F, including wave RAM at 0x20 */
    int powered;
    GbChannel ch[4];
    int frame_step;
    uint32_t sample_phase; /* fixed-point accumulator toward the next output sample */
    int32_t acc_left, acc_right;
    uint32_t acc_count;
    double hp_left, hp_right;
    int16_t buffer[GB_AUDIO_CAPACITY * 2];
    size_t read_pos, write_pos; /* in frames, wrapping */
} GbApu;

struct Gb {
    /* CPU */
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    int ime, ei_pending, halted, halt_bug, locked;
    char lock_reason[96];

    /* Memory and IO */
    uint8_t vram[0x2000];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    uint8_t ie, if_reg;

    /* Timer */
    uint16_t div_counter;
    uint8_t tima, tma, tac;
    int tima_reload;      /* T-cycles until TIMA reloads from TMA after overflow */

    /* Serial */
    uint8_t sb, sc;
    int serial_timer;
    char serial_out[GB_SERIAL_CAPACITY + 1];
    size_t serial_length;

    /* OAM DMA */
    int dma_active;
    int dma_delay;
    uint16_t dma_source;
    int dma_index;

    /* Joypad */
    uint8_t joyp_select;
    uint8_t buttons;      /* bit per GbButton, 1 = held */

    uint64_t cycles;

    GbPpu ppu;
    GbApu apu;
    GbCart cart;
};

/* Advance every component by one M-cycle (4 T-cycles). */
void gb_tick(Gb *gb);
uint8_t gb_bus_read(Gb *gb, uint16_t address);
void gb_bus_write(Gb *gb, uint16_t address, uint8_t value);

void gb_cpu_reset(Gb *gb);
void gb_cpu_step(Gb *gb);

void gb_ppu_reset(Gb *gb);
void gb_ppu_step(Gb *gb, int dots);
uint8_t gb_ppu_read(Gb *gb, uint16_t address);
void gb_ppu_write(Gb *gb, uint16_t address, uint8_t value);

void gb_apu_reset(Gb *gb);
void gb_apu_step(Gb *gb, int cycles);
void gb_apu_frame_sequencer(Gb *gb);
uint8_t gb_apu_read(Gb *gb, uint16_t address);
void gb_apu_write(Gb *gb, uint16_t address, uint8_t value);

int gb_cart_load(GbCart *cart, const uint8_t *rom, size_t length, char *error, size_t capacity);
void gb_cart_free(GbCart *cart);
void gb_cart_reset(GbCart *cart);
uint8_t gb_cart_read(GbCart *cart, uint16_t address);
void gb_cart_write(GbCart *cart, uint16_t address, uint8_t value);
void gb_cart_tick(GbCart *cart, int cycles);

#endif
