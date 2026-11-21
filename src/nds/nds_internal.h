#ifndef NACRE_NDS_INTERNAL_H
#define NACRE_NDS_INTERNAL_H

/* Machine state shared by the DS core's source files. Not part of the API. */

#include "nds.h"

#define NDS_AUDIO_RING 16384u /* stereo frames, about a third of a second */
#define NDS_MAX_ROM (512u << 20)
#define NDS_HEADER_SIZE 0x200u

/* Main RAM and the ARM7's private RAM, where direct boot may place binaries. */
#define NDS_MAIN_RAM_BASE 0x02000000u
#define NDS_MAIN_RAM_SIZE (4u << 20)
#define NDS_ARM7_WRAM_BASE 0x037F8000u /* shared WRAM mapped to the ARM7, then its own 64 KiB */
#define NDS_ARM7_WRAM_END 0x03810000u

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

struct Nds {
    uint8_t *rom;
    size_t rom_size;
    NdsHeader header;
    char title[128];

    uint64_t cycles;        /* ARM7 cycles since power-on */
    uint32_t frame_cycle;   /* position within the current frame */
    uint32_t frame_count;
    uint64_t pace_target;   /* cycle count advance_ns is running toward */
    uint64_t pace_fraction; /* its remainder, in nanosecond-cycles */
    uint64_t sample_fraction; /* fractional audio frame, in units of 1/NDS_ARM7_HZ */

    uint16_t buttons;       /* bit per NdsButton, 1 = held */
    int touch_x, touch_y, touching;

    uint32_t framebuffer[NDS_FRAME_WIDTH * NDS_FRAME_HEIGHT];

    int16_t audio[NDS_AUDIO_RING * 2];
    size_t audio_read, audio_write; /* ring positions; equal when empty */
};

/* nds_cart.c */
int nds_parse_header(const uint8_t *rom, size_t length, NdsHeader *header, char *error,
                     size_t error_capacity);
void nds_banner_title(const uint8_t *rom, size_t length, const NdsHeader *header, char *out,
                      size_t capacity);

#endif
