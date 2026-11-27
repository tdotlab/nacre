#ifndef NACRE_NDS_H
#define NACRE_NDS_H

/* Nintendo DS emulation core. C99, no platform dependencies. Frontends own files,
 * windows, input devices, and audio devices; the core takes ROM and save bytes and
 * produces XRGB8888 pixels and audio samples. Games boot directly: no Nintendo
 * BIOS or firmware is used or needed (see docs/NDS.md). */

#include <stddef.h>
#include <stdint.h>

#define NDS_SCREEN_WIDTH 256
#define NDS_SCREEN_HEIGHT 192
/* The framebuffer holds both screens, top over bottom. */
#define NDS_FRAME_WIDTH NDS_SCREEN_WIDTH
#define NDS_FRAME_HEIGHT (2 * NDS_SCREEN_HEIGHT)
/* The ARM7 clock; the ARM9 runs at twice this. */
#define NDS_ARM7_HZ 33513982u
/* 263 lines of 355 dots, 6 ARM7 cycles a dot: 59.8261 frames a second. */
#define NDS_CYCLES_PER_FRAME 560190u
#define NDS_AUDIO_SAMPLE_RATE 48000u

/* In the order of the KEYINPUT bits, then X and Y from EXTKEYIN. */
typedef enum {
    NDS_BUTTON_A = 0,
    NDS_BUTTON_B,
    NDS_BUTTON_SELECT,
    NDS_BUTTON_START,
    NDS_BUTTON_RIGHT,
    NDS_BUTTON_LEFT,
    NDS_BUTTON_UP,
    NDS_BUTTON_DOWN,
    NDS_BUTTON_R,
    NDS_BUTTON_L,
    NDS_BUTTON_X,
    NDS_BUTTON_Y,
    NDS_BUTTON_COUNT
} NdsButton;

typedef struct Nds Nds;

Nds *nds_create(void);
void nds_destroy(Nds *nds);

/* Validates the cartridge header and copies the ROM. On failure the machine keeps
 * its previous cartridge and state. */
int nds_load_rom(Nds *nds, const uint8_t *rom, size_t length, char *error, size_t error_capacity);
int nds_has_rom(const Nds *nds);
void nds_reset(Nds *nds);

/* Runs at least `cycles` ARM7 cycles. Returns 0 if the machine faulted; the error
 * text explains why. */
int nds_run_cycles(Nds *nds, uint32_t cycles, char *error, size_t error_capacity);

/* Runs the machine for `elapsed_ns` of wall time at `speed_percent` (100 = real
 * speed). Elapsed time is capped at 250 ms and fractional cycles carry over. */
int nds_advance_ns(Nds *nds, uint64_t elapsed_ns, unsigned speed_percent, char *error,
                   size_t error_capacity);

/* NDS_FRAME_WIDTH x NDS_FRAME_HEIGHT pixels, 0x00RRGGBB. Updated once per frame. */
const uint32_t *nds_framebuffer(const Nds *nds);
uint32_t nds_frame_count(const Nds *nds);

void nds_set_button(Nds *nds, NdsButton button, int pressed);
/* A touch on the bottom screen, in its pixels (0..255, 0..191). */
void nds_set_touch(Nds *nds, int x, int y, int pressed);

/* Sets the real-time clock from Unix seconds; the time the game has set
 * itself is kept as an offset. The DS keeps local time, so pass seconds
 * shifted by the host's UTC offset to show local time. Before any call the
 * clock starts at 2000-01-01 00:00. */
void nds_set_wall_clock(Nds *nds, int64_t unix_seconds);

/* The cartridge's backup chip. AUTO detects it from the save file's size or
 * from how the game uses it; any other type is the save_type override. */
typedef enum {
    NDS_SAVE_AUTO,
    NDS_SAVE_NONE,
    NDS_SAVE_EEPROM_512,
    NDS_SAVE_EEPROM_8K,
    NDS_SAVE_EEPROM_64K,
    NDS_SAVE_EEPROM_128K,
    NDS_SAVE_FRAM_32K,
    NDS_SAVE_FLASH_256K,
    NDS_SAVE_FLASH_512K,
    NDS_SAVE_FLASH_1M,
    NDS_SAVE_TYPE_COUNT
} NdsSaveType;

/* Names as the save_type option spells them ("auto", "eeprom8k", "flash512k"...);
 * from_name returns NDS_SAVE_TYPE_COUNT for an unknown name. */
const char *nds_save_type_name(NdsSaveType type);
NdsSaveType nds_save_type_from_name(const char *name);
void nds_set_save_type(Nds *nds, NdsSaveType type);
/* The chip in use; AUTO until detection settles. */
NdsSaveType nds_save_type(const Nds *nds);
/* 0 until the type is known. */
size_t nds_save_size(const Nds *nds);
size_t nds_save_write(const Nds *nds, uint8_t *out, size_t capacity);
/* Takes a save file; its size picks the chip unless the override is set.
 * Returns 0 for a size no chip has. Call after nds_load_rom. */
int nds_save_load(Nds *nds, const uint8_t *data, size_t size);
int nds_save_dirty(const Nds *nds);
void nds_save_mark_clean(Nds *nds);

/* Interleaved stereo int16 at NDS_AUDIO_SAMPLE_RATE. Returns frames copied. The
 * internal buffer holds about a third of a second; older samples are dropped. */
size_t nds_audio_read(Nds *nds, int16_t *stereo, size_t max_frames);
size_t nds_audio_available(const Nds *nds);
void nds_audio_clear(Nds *nds);

/* The English banner title's first line (UTF-8), else the header title. */
const char *nds_title(const Nds *nds);
/* The four-character game code ("AMCE"), or "" with no cartridge. */
const char *nds_game_code(const Nds *nds);

/* Returns 1 if the bytes start with a valid DS cartridge header: the logo and
 * header checksums both match their fields. Needs the first 0x160 bytes. */
int nds_rom_looks_valid(const uint8_t *rom, size_t length);

/* CRC-16 as the DS BIOS computes it (reflected polynomial 0xA001). */
uint16_t nds_crc16(uint16_t crc, const uint8_t *data, size_t length);

#endif
