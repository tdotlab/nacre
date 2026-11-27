/* The DS machine: lifetime, loading, pacing, input, the audio ring. */

#include "nds_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Nds *nds_create(void) {
    Nds *nds = (Nds *)calloc(1, sizeof(Nds));
    if (nds) nds_reset(nds);
    return nds;
}

void nds_destroy(Nds *nds) {
    if (!nds) return;
    free(nds->rom);
    free(nds);
}

int nds_load_rom(Nds *nds, const uint8_t *rom, size_t length, char *error, size_t error_capacity) {
    NdsHeader header;
    uint8_t *copy;
    if (!nds_parse_header(rom, length, &header, error, error_capacity)) return 0;
    copy = (uint8_t *)malloc(length);
    if (!copy) {
        snprintf(error, error_capacity, "Not enough memory for this cartridge.");
        return 0;
    }
    memcpy(copy, rom, length);
    free(nds->rom);
    nds->rom = copy;
    nds->rom_size = length;
    nds->header = header;
    nds_banner_title(rom, length, &header, nds->title, sizeof nds->title);
    if (!nds->title[0]) snprintf(nds->title, sizeof nds->title, "%s", header.title);
    nds_reset(nds);
    return 1;
}

int nds_has_rom(const Nds *nds) { return nds->rom != NULL; }

void nds_reset(Nds *nds) {
    nds->rtc.base += (int64_t)(nds->arm7.cycles / NDS_ARM7_HZ); /* the clock keeps running */
    nds->cycles = 0;
    nds->frame_cycle = 0;
    nds->frame_count = 0;
    nds->line = 0;
    nds->line_cycle = 0;
    nds->pace_target = 0;
    nds->pace_fraction = 0;
    nds->sample_fraction = 0;
    nds->audio_read = nds->audio_write = 0;
    memset(nds->framebuffer, 0, sizeof nds->framebuffer);
    nds_mem_reset(nds);
    nds_io_reset(nds);
    nds_arm_reset(&nds->arm9, nds, 1);
    nds_arm_reset(&nds->arm7, nds, 0);
    nds_cp15_reset(nds);
    nds_timer_reset(nds);
    nds_dma_reset(nds);
    nds_cart_reset(nds);
    nds_spi_reset(nds);
    nds_firmware_build(nds);
    nds_rtc_reset(nds);
    nds_bios_build(nds);
    if (nds->rom) nds_direct_boot(nds);
}

/* Queues one stereo frame, dropping the oldest when the ring is full. */
static void push_audio(Nds *nds, int16_t left, int16_t right) {
    size_t next = (nds->audio_write + 1) & (NDS_AUDIO_RING - 1);
    if (next == nds->audio_read) nds->audio_read = (nds->audio_read + 1) & (NDS_AUDIO_RING - 1);
    nds->audio[nds->audio_write * 2] = left;
    nds->audio[nds->audio_write * 2 + 1] = right;
    nds->audio_write = next;
}

/* Emits the 48 kHz frames that fall within `cycles` of emulated time. */
static void produce_audio(Nds *nds, uint32_t cycles) {
    uint64_t total = (uint64_t)cycles * NDS_AUDIO_SAMPLE_RATE + nds->sample_fraction;
    uint64_t frames = total / NDS_ARM7_HZ;
    nds->sample_fraction = total % NDS_ARM7_HZ;
    while (frames--) push_audio(nds, 0, 0);
}

/* The CPUs run in short slices between display events, the ARM9 first (two of
 * its cycles per ARM7 cycle). Slices are short so IPC and IRQs between the two
 * CPUs land close to when they happen. */
#define NDS_SLICE 64u

/* DISPSTAT: bit 3 VBlank IRQ enable, 4 HBlank, 5 VCount match. */
static void display_irq(Nds *nds, uint32_t enable_bit, uint32_t irq) {
    for (int cpu = 0; cpu < 2; cpu++)
        if (nds->dispstat[cpu] & enable_bit) nds_irq_raise(nds, cpu, irq);
}

static void start_line(Nds *nds) {
    if (nds->line == NDS_SCREEN_HEIGHT) {
        display_irq(nds, 1u << 3, NDS_IRQ_VBLANK);
        nds_dma_trigger(nds, 0, NDS_DMA_VBLANK);
        nds_dma_trigger(nds, 1, NDS_DMA_VBLANK);
    }
    if (nds->line == 0) nds_dma_trigger(nds, 0, NDS_DMA_DISPLAY_START);
    if (nds->line < NDS_SCREEN_HEIGHT) nds_dma_trigger(nds, 0, NDS_DMA_MAIN_DISPLAY);
    for (int cpu = 0; cpu < 2; cpu++) {
        uint32_t ds = nds->dispstat[cpu], target = (ds >> 8) | ((ds & 0x80) << 1);
        if ((ds & (1u << 5)) && nds->line == target) nds_irq_raise(nds, cpu, NDS_IRQ_VCOUNT);
    }
}

/* The next time something besides the display needs handling: a timer
 * overflow that raises an IRQ, or the end of a cart transfer. */
static uint64_t next_event(const Nds *nds) {
    uint64_t at = nds_timer_next_event(nds, 0), t = nds_timer_next_event(nds, 1);
    if (t < at) at = t;
    if (nds->cart.pending && nds->cart.ready < at) at = nds->cart.ready;
    return at;
}

int nds_run_cycles(Nds *nds, uint32_t cycles, char *error, size_t error_capacity) {
    (void)error;
    (void)error_capacity;
    if (!nds->rom) return 1;
    while (cycles) {
        uint32_t event = nds->line_cycle < NDS_HBLANK_CYCLE ? NDS_HBLANK_CYCLE : NDS_LINE_CYCLES;
        uint32_t step = event - nds->line_cycle;
        uint64_t at = next_event(nds);
        if (step > NDS_SLICE) step = NDS_SLICE;
        if (step > cycles) step = cycles;
        if (at > nds->cycles && at - nds->cycles < step) step = (uint32_t)(at - nds->cycles);
        nds_arm_run(&nds->arm9, (nds->cycles + step) * 2);
        nds_arm_run(&nds->arm7, nds->cycles + step);
        produce_audio(nds, step);
        nds->cycles += step;
        nds->frame_cycle += step;
        nds->line_cycle += step;
        cycles -= step;
        nds_timer_update(nds, 0, nds->cycles);
        nds_timer_update(nds, 1, nds->cycles);
        nds_cart_update(nds, nds->cycles);
        if (nds->line_cycle == NDS_HBLANK_CYCLE) {
            if (nds->line < NDS_SCREEN_HEIGHT) {
                nds_render_line(nds, nds->line);
                nds_dma_trigger(nds, 0, NDS_DMA_HBLANK); /* paused during VBlank */
            }
            display_irq(nds, 1u << 4, NDS_IRQ_HBLANK);
        } else if (nds->line_cycle == NDS_LINE_CYCLES) {
            nds->line_cycle = 0;
            if (++nds->line == NDS_LINES) {
                nds->line = 0;
                nds->frame_cycle = 0;
                nds->frame_count++;
            }
            start_line(nds);
        }
    }
    return 1;
}

int nds_advance_ns(Nds *nds, uint64_t elapsed_ns, unsigned speed_percent, char *error,
                   size_t error_capacity) {
    uint64_t scaled;
    if (elapsed_ns > 250000000u) elapsed_ns = 250000000u; /* resume calmly after a stall */
    if (speed_percent == 0) speed_percent = 100;
    if (speed_percent > 1000) speed_percent = 1000;
    scaled = elapsed_ns * NDS_ARM7_HZ / 100u * speed_percent + nds->pace_fraction;
    nds->pace_target += scaled / 1000000000u;
    nds->pace_fraction = scaled % 1000000000u;
    if (nds->pace_target <= nds->cycles) return 1;
    return nds_run_cycles(nds, (uint32_t)(nds->pace_target - nds->cycles), error, error_capacity);
}

const uint32_t *nds_framebuffer(const Nds *nds) { return nds->framebuffer; }
uint32_t nds_frame_count(const Nds *nds) { return nds->frame_count; }

void nds_set_button(Nds *nds, NdsButton button, int pressed) {
    if ((unsigned)button >= NDS_BUTTON_COUNT) return;
    if (pressed)
        nds->buttons |= (uint16_t)(1u << button);
    else
        nds->buttons &= (uint16_t)~(1u << button);
}

void nds_set_touch(Nds *nds, int x, int y, int pressed) {
    nds->touching = pressed && x >= 0 && x < NDS_SCREEN_WIDTH && y >= 0 && y < NDS_SCREEN_HEIGHT;
    if (nds->touching) {
        nds->touch_x = x;
        nds->touch_y = y;
    }
}

size_t nds_audio_available(const Nds *nds) {
    return (nds->audio_write - nds->audio_read) & (NDS_AUDIO_RING - 1);
}

size_t nds_audio_read(Nds *nds, int16_t *stereo, size_t max_frames) {
    size_t n = 0;
    while (n < max_frames && nds->audio_read != nds->audio_write) {
        stereo[n * 2] = nds->audio[nds->audio_read * 2];
        stereo[n * 2 + 1] = nds->audio[nds->audio_read * 2 + 1];
        nds->audio_read = (nds->audio_read + 1) & (NDS_AUDIO_RING - 1);
        n++;
    }
    return n;
}

void nds_audio_clear(Nds *nds) { nds->audio_read = nds->audio_write; }

const char *nds_title(const Nds *nds) { return nds->rom ? nds->title : ""; }
const char *nds_game_code(const Nds *nds) { return nds->rom ? nds->header.game_code : ""; }
