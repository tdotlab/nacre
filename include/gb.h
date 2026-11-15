#ifndef NACRE_GB_H
#define NACRE_GB_H

/* Original Game Boy (DMG) emulation core. C99, no platform dependencies.
 * Frontends own files, windows, input devices, and audio devices; the core takes
 * ROM and save bytes and produces shade indices and audio samples. */

#include <stddef.h>
#include <stdint.h>

#define GB_SCREEN_WIDTH 160
#define GB_SCREEN_HEIGHT 144
#define GB_CLOCK_HZ 4194304u
#define GB_CYCLES_PER_FRAME 70224u
#define GB_AUDIO_SAMPLE_RATE 48000u

typedef enum {
    GB_BUTTON_RIGHT = 0,
    GB_BUTTON_LEFT,
    GB_BUTTON_UP,
    GB_BUTTON_DOWN,
    GB_BUTTON_A,
    GB_BUTTON_B,
    GB_BUTTON_SELECT,
    GB_BUTTON_START,
    GB_BUTTON_COUNT
} GbButton;

typedef struct Gb Gb;

Gb *gb_create(void);
void gb_destroy(Gb *gb);

/* Validates the cartridge header and copies the ROM. On failure the machine keeps
 * its previous cartridge and state. Starts from the post-boot-ROM register state;
 * no Nintendo boot ROM is used or needed. */
int gb_load_rom(Gb *gb, const uint8_t *rom, size_t length, char *error, size_t error_capacity);
int gb_has_rom(const Gb *gb);
void gb_reset(Gb *gb);

/* Runs at least `cycles` T-cycles (4,194,304 per second). Returns 0 if the CPU
 * locked up (illegal opcode); the error text explains why. */
int gb_run_cycles(Gb *gb, uint32_t cycles, char *error, size_t error_capacity);

/* Runs the machine for `elapsed_ns` of wall time at `speed_percent` (100 = real
 * speed). Elapsed time is capped at 250 ms and fractional cycles carry over, so
 * calling this every display frame keeps long-run timing exact. */
int gb_advance_ns(Gb *gb, uint64_t elapsed_ns, unsigned speed_percent, char *error,
                  size_t error_capacity);

/* 160x144 shade indices, 0 = lightest .. 3 = darkest. Updated once per frame. */
const uint8_t *gb_framebuffer(const Gb *gb);
uint32_t gb_frame_count(const Gb *gb);

void gb_set_button(Gb *gb, GbButton button, int pressed);

/* Interleaved stereo int16 at GB_AUDIO_SAMPLE_RATE. Returns frames copied. The
 * internal buffer holds about a third of a second; older samples are dropped. */
size_t gb_audio_read(Gb *gb, int16_t *stereo, size_t max_frames);
size_t gb_audio_available(const Gb *gb);
void gb_audio_clear(Gb *gb);

/* Cartridge information. */
const char *gb_title(const Gb *gb);
int gb_has_battery(const Gb *gb);

/* Battery save: cartridge RAM, followed by the 48-byte RTC block for MBC3 timer
 * carts (the layout used by common emulators). `gb_save_dirty` is set by writes
 * to cartridge RAM or the clock and cleared by `gb_save_mark_clean`. */
size_t gb_save_size(const Gb *gb);
size_t gb_save_write(const Gb *gb, uint8_t *out, size_t capacity);
int gb_save_load(Gb *gb, const uint8_t *data, size_t length);
int gb_save_dirty(const Gb *gb);
void gb_save_mark_clean(Gb *gb);
/* Unix seconds, used to advance the MBC3 clock across sessions; 0 leaves it frozen. */
void gb_set_wall_clock(Gb *gb, int64_t unix_seconds);

/* Save states: a snapshot of the whole running machine (CPU, memory, video,
 * sound, cartridge RAM and mapper). `gb_state_size` is 0 with no ROM loaded.
 * `gb_state_save` writes that many bytes and returns the count, or 0 on failure.
 * `gb_state_load` returns 1 on success; a state from another game, another build
 * or a truncated file is rejected and leaves the machine untouched. States are
 * not portable between builds. Loading discards queued audio and marks the
 * battery save dirty. */
size_t gb_state_size(const Gb *gb);
size_t gb_state_save(const Gb *gb, uint8_t *out, size_t capacity);
int gb_state_load(Gb *gb, const uint8_t *data, size_t length);

/* Bytes the program sent out of the link port (test ROMs print results here). */
const char *gb_serial_output(const Gb *gb, size_t *length);

/* Returns 1 if the bytes carry a Game Boy cartridge header (Nintendo logo check). */
int gb_rom_looks_valid(const uint8_t *rom, size_t length);

#endif
