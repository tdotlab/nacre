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
    nds->cycles = 0;
    nds->frame_cycle = 0;
    nds->frame_count = 0;
    nds->pace_target = 0;
    nds->pace_fraction = 0;
    nds->sample_fraction = 0;
    nds->audio_read = nds->audio_write = 0;
    memset(nds->framebuffer, 0, sizeof nds->framebuffer);
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

int nds_run_cycles(Nds *nds, uint32_t cycles, char *error, size_t error_capacity) {
    (void)error;
    (void)error_capacity;
    if (!nds->rom) return 1;
    while (cycles) {
        uint32_t step = NDS_CYCLES_PER_FRAME - nds->frame_cycle;
        if (step > cycles) step = cycles;
        produce_audio(nds, step);
        nds->cycles += step;
        nds->frame_cycle += step;
        cycles -= step;
        if (nds->frame_cycle == NDS_CYCLES_PER_FRAME) {
            nds->frame_cycle = 0;
            nds->frame_count++;
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
