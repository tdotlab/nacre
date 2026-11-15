/* Game Boy machine: memory map, timer, serial, OAM DMA, joypad, and run loop. */

#include "gb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity > 0) {
        snprintf(error, capacity, "%s", message);
    }
}

Gb *gb_create(void) {
    Gb *gb = (Gb *)calloc(1, sizeof(Gb));
    if (!gb) return NULL;
    gb_reset(gb);
    return gb;
}

void gb_destroy(Gb *gb) {
    if (!gb) return;
    gb_cart_free(&gb->cart);
    free(gb);
}

int gb_has_rom(const Gb *gb) { return gb && gb->cart.rom != NULL; }

void gb_reset(Gb *gb) {
    gb_cpu_reset(gb);
    memset(gb->vram, 0, sizeof gb->vram);
    memset(gb->wram, 0, sizeof gb->wram);
    memset(gb->oam, 0, sizeof gb->oam);
    memset(gb->hram, 0, sizeof gb->hram);
    gb->ie = 0;
    gb->if_reg = 0x01; /* the boot ROM leaves VBlank requested */
    gb->div_counter = 0xABCC; /* DIV reads $AB after the DMG boot ROM */
    gb->tima = 0;
    gb->tma = 0;
    gb->tac = 0;
    gb->tima_reload = 0;
    gb->sb = 0;
    gb->sc = 0;
    gb->serial_timer = 0;
    gb->serial_length = 0;
    gb->serial_out[0] = '\0';
    gb->dma_active = 0;
    gb->dma_delay = 0;
    gb->dma_index = 0;
    gb->dma_source = 0;
    gb->joyp_select = 0x30;
    gb->buttons = 0;
    gb->cycles = 0;
    gb->pace_target = 0;
    gb->pace_fraction = 0;
    gb_ppu_reset(gb);
    gb_apu_reset(gb);
    if (gb->cart.rom) gb_cart_reset(&gb->cart);
}

int gb_load_rom(Gb *gb, const uint8_t *rom, size_t length, char *error, size_t error_capacity) {
    GbCart candidate;
    memset(&candidate, 0, sizeof candidate);
    if (!gb_cart_load(&candidate, rom, length, error, error_capacity)) {
        return 0;
    }
    candidate.wall_clock = gb->cart.wall_clock;
    gb_cart_free(&gb->cart);
    gb->cart = candidate;
    gb_reset(gb);
    return 1;
}

/* ---- timer ---------------------------------------------------------------- */

static const uint16_t timer_bits[4] = {1u << 9, 1u << 3, 1u << 5, 1u << 7};

static int timer_signal(const Gb *gb, uint16_t counter) {
    return (gb->tac & 4) && (counter & timer_bits[gb->tac & 3]);
}

static void timer_increment(Gb *gb) {
    gb->tima++;
    if (gb->tima == 0) {
        gb->tima_reload = 4; /* TIMA reads $00 for one M-cycle before the reload */
    }
}

static void set_div_counter(Gb *gb, uint16_t value) {
    uint16_t old = gb->div_counter;
    gb->div_counter = value;
    if (timer_signal(gb, old) && !timer_signal(gb, value)) timer_increment(gb);
    if ((old & 0x1000) && !(value & 0x1000)) gb_apu_frame_sequencer(gb);
}

static void timer_step(Gb *gb) {
    if (gb->tima_reload) {
        gb->tima_reload--;
        if (gb->tima_reload == 0) {
            gb->tima = gb->tma;
            gb->if_reg |= GB_IRQ_TIMER;
        }
    }
    set_div_counter(gb, (uint16_t)(gb->div_counter + 1));
}

/* ---- the M-cycle clock ------------------------------------------------------ */

void gb_tick(Gb *gb) {
    int i;
    for (i = 0; i < 4; i++) timer_step(gb);

    if (gb->serial_timer > 0) {
        gb->serial_timer -= 4;
        if (gb->serial_timer <= 0) {
            gb->serial_timer = 0;
            gb->sb = 0xFF; /* nothing is connected, so the line reads high */
            gb->sc &= 0x7F;
            gb->if_reg |= GB_IRQ_SERIAL;
        }
    }

    if (gb->dma_active) {
        if (gb->dma_delay > 0) {
            gb->dma_delay--;
        } else {
            uint16_t src = (uint16_t)(gb->dma_source + gb->dma_index);
            uint8_t value;
            if (src >= 0xE000) src = (uint16_t)(src - 0x2000);
            gb->dma_active = 0; /* let the read see memory normally */
            value = gb_bus_read(gb, src);
            gb->dma_active = 1;
            gb->oam[gb->dma_index] = value;
            gb->dma_index++;
            if (gb->dma_index >= 0xA0) gb->dma_active = 0;
        }
    }

    gb_ppu_step(gb, 4);
    gb_apu_step(gb, 4);
    gb_cart_tick(&gb->cart, 4);
    gb->cycles += 4;
}

/* ---- joypad ------------------------------------------------------------------ */

static uint8_t joypad_read(const Gb *gb) {
    uint8_t low = 0x0F;
    if (!(gb->joyp_select & 0x10)) low &= (uint8_t)~(gb->buttons & 0x0F);
    if (!(gb->joyp_select & 0x20)) low &= (uint8_t)~((gb->buttons >> 4) & 0x0F);
    return (uint8_t)(0xC0 | gb->joyp_select | low);
}

void gb_set_button(Gb *gb, GbButton button, int pressed) {
    uint8_t bit;
    uint8_t before;
    if (!gb || (int)button < 0 || button >= GB_BUTTON_COUNT) return;
    bit = (uint8_t)(1u << button);
    before = joypad_read(gb);
    if (pressed) gb->buttons |= bit;
    else gb->buttons &= (uint8_t)~bit;
    /* A high-to-low transition on a selected input line requests the interrupt. */
    if ((before & ~joypad_read(gb)) & 0x0F) gb->if_reg |= GB_IRQ_JOYPAD;
}

/* ---- bus ------------------------------------------------------------------------ */

uint8_t gb_bus_read(Gb *gb, uint16_t a) {
    if (a < 0x8000) return gb_cart_read(&gb->cart, a);
    if (a < 0xA000) return gb->vram[a - 0x8000];
    if (a < 0xC000) return gb_cart_read(&gb->cart, a);
    if (a < 0xE000) return gb->wram[a - 0xC000];
    if (a < 0xFE00) return gb->wram[a - 0xE000];
    if (a < 0xFEA0) return gb->dma_active ? 0xFF : gb->oam[a - 0xFE00];
    if (a < 0xFF00) return 0x00;
    if (a >= 0xFF80 && a < 0xFFFF) return gb->hram[a - 0xFF80];
    if (a == 0xFFFF) return gb->ie;

    switch (a) {
    case 0xFF00: return joypad_read(gb);
    case 0xFF01: return gb->sb;
    case 0xFF02: return (uint8_t)(gb->sc | 0x7E);
    case 0xFF04: return (uint8_t)(gb->div_counter >> 8);
    case 0xFF05: return gb->tima;
    case 0xFF06: return gb->tma;
    case 0xFF07: return (uint8_t)(gb->tac | 0xF8);
    case 0xFF0F: return (uint8_t)(gb->if_reg | 0xE0);
    default: break;
    }
    if (a >= 0xFF10 && a <= 0xFF3F) return gb_apu_read(gb, a);
    if (a >= 0xFF40 && a <= 0xFF4B) return gb_ppu_read(gb, a);
    return 0xFF;
}

void gb_bus_write(Gb *gb, uint16_t a, uint8_t v) {
    if (a < 0x8000) { gb_cart_write(&gb->cart, a, v); return; }
    if (a < 0xA000) { gb->vram[a - 0x8000] = v; return; }
    if (a < 0xC000) { gb_cart_write(&gb->cart, a, v); return; }
    if (a < 0xE000) { gb->wram[a - 0xC000] = v; return; }
    if (a < 0xFE00) { gb->wram[a - 0xE000] = v; return; }
    if (a < 0xFEA0) { if (!gb->dma_active) gb->oam[a - 0xFE00] = v; return; }
    if (a < 0xFF00) return;
    if (a >= 0xFF80 && a < 0xFFFF) { gb->hram[a - 0xFF80] = v; return; }
    if (a == 0xFFFF) { gb->ie = v; return; }

    switch (a) {
    case 0xFF00: gb->joyp_select = (uint8_t)(v & 0x30); return;
    case 0xFF01: gb->sb = v; return;
    case 0xFF02:
        gb->sc = (uint8_t)(v & 0x81);
        if ((v & 0x81) == 0x81) {
            if (gb->serial_length < GB_SERIAL_CAPACITY) {
                gb->serial_out[gb->serial_length++] = (char)gb->sb;
                gb->serial_out[gb->serial_length] = '\0';
            }
            gb->serial_timer = 8 * 512; /* 8192 Hz internal clock */
        }
        return;
    case 0xFF04: set_div_counter(gb, 0); return;
    case 0xFF05:
        if (gb->tima_reload) gb->tima_reload = 0; /* a write during the delay cancels the reload */
        gb->tima = v;
        return;
    case 0xFF06: gb->tma = v; return;
    case 0xFF07: {
        int before = timer_signal(gb, gb->div_counter);
        gb->tac = (uint8_t)(v & 7);
        if (before && !timer_signal(gb, gb->div_counter)) timer_increment(gb);
        return;
    }
    case 0xFF0F: gb->if_reg = (uint8_t)(v & 0x1F); return;
    case 0xFF46:
        gb->dma_active = 1;
        gb->dma_delay = 1;
        gb->dma_index = 0;
        gb->dma_source = (uint16_t)(v << 8);
        gb_ppu_write(gb, a, v);
        return;
    default: break;
    }
    if (a >= 0xFF10 && a <= 0xFF3F) { gb_apu_write(gb, a, v); return; }
    if (a >= 0xFF40 && a <= 0xFF4B) { gb_ppu_write(gb, a, v); return; }
}

/* ---- run loop and accessors ------------------------------------------------------- */

int gb_run_cycles(Gb *gb, uint32_t cycles, char *error, size_t error_capacity) {
    uint64_t target;
    if (!gb->cart.rom) {
        set_error(error, error_capacity, "No Game Boy cartridge is loaded.");
        return 0;
    }
    target = gb->cycles + cycles;
    while (gb->cycles < target) {
        gb_cpu_step(gb);
        if (gb->locked) {
            set_error(error, error_capacity, gb->lock_reason);
            return 0;
        }
    }
    return 1;
}

int gb_advance_ns(Gb *gb, uint64_t elapsed_ns, unsigned speed_percent, char *error,
                  size_t error_capacity) {
    uint64_t scaled;
    if (elapsed_ns > 250000000u) elapsed_ns = 250000000u; /* resume calmly after a stall */
    if (speed_percent == 0) speed_percent = 100;
    if (speed_percent > 1000) speed_percent = 1000;
    scaled = elapsed_ns * GB_CLOCK_HZ / 100u * speed_percent + gb->pace_fraction;
    gb->pace_target += scaled / 1000000000u;
    gb->pace_fraction = scaled % 1000000000u;
    if (gb->pace_target <= gb->cycles) return 1;
    return gb_run_cycles(gb, (uint32_t)(gb->pace_target - gb->cycles), error, error_capacity);
}

const uint8_t *gb_framebuffer(const Gb *gb) { return gb->ppu.front; }
uint32_t gb_frame_count(const Gb *gb) { return gb->ppu.frame_count; }

const char *gb_serial_output(const Gb *gb, size_t *length) {
    if (length) *length = gb->serial_length;
    return gb->serial_out;
}

const char *gb_title(const Gb *gb) { return gb->cart.title; }
int gb_has_battery(const Gb *gb) { return gb->cart.has_battery; }

size_t gb_audio_available(const Gb *gb) {
    return (gb->apu.write_pos - gb->apu.read_pos) & (GB_AUDIO_CAPACITY - 1);
}

size_t gb_audio_read(Gb *gb, int16_t *stereo, size_t max_frames) {
    size_t n = 0;
    while (n < max_frames && gb->apu.read_pos != gb->apu.write_pos) {
        stereo[n * 2] = gb->apu.buffer[gb->apu.read_pos * 2];
        stereo[n * 2 + 1] = gb->apu.buffer[gb->apu.read_pos * 2 + 1];
        gb->apu.read_pos = (gb->apu.read_pos + 1) & (GB_AUDIO_CAPACITY - 1);
        n++;
    }
    return n;
}

void gb_audio_clear(Gb *gb) { gb->apu.read_pos = gb->apu.write_pos; }
