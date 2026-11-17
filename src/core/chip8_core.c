/* CHIP-8 behind the core interface. The tone is rendered in step with the
 * emulated 60 Hz timer, so CHIP-8 sound flows through the same audio path as
 * every other core instead of a device callback of its own. */

#include "nacre_core.h"

#include "audio.h"
#include "chip8.h"
#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHIP8_SEED 0x4E414352u
#define MAX_PROGRAM (CHIP8_MEMORY_SIZE - CHIP8_PROGRAM_START)
#define SAMPLES_PER_TICK (CHIP8_AUDIO_SAMPLE_RATE / CHIP8_TIMER_HZ)
#define RING_FRAMES 16384u /* about a third of a second */
#define PIXELS (CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT)

#define STATE_MAGIC 0x54533843u /* "C8ST" */
#define STATE_VERSION 1u

typedef struct {
    NacreHost host;
    Chip8 machine;
    Chip8Scheduler scheduler;
    uint8_t program[MAX_PROGRAM];
    size_t program_size;
    uint32_t program_hash;
    uint32_t cpu_hz;
    int loaded;
    int running;
    int faulted;
    unsigned speed;
    uint64_t speed_carry; /* leftover ns * 100 from scaling */
    char error[256];

    uint8_t shown[PIXELS];
    NacreVideoFrame frame;

    Chip8AudioOscillator oscillator;
    int16_t ring[RING_FRAMES * 2];
    size_t ring_read;
    size_t ring_count;
} Chip8Core;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t machine_size;
    uint32_t program_hash;
} StateHeader;

static uint32_t fnv1a(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

static void ring_push(Chip8Core *core, const int16_t *mono, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (core->ring_count == RING_FRAMES) { /* full: drop the oldest frame */
            core->ring_read = (core->ring_read + 1) % RING_FRAMES;
            core->ring_count--;
        }
        size_t at = (core->ring_read + core->ring_count) % RING_FRAMES;
        core->ring[at * 2] = mono[i];
        core->ring[at * 2 + 1] = mono[i];
        core->ring_count++;
    }
}

/* Runs after each 60 Hz tick: the next sixtieth of a second sounds if the timer
 * is still running. */
static void on_timer(Chip8 *machine, void *userdata) {
    Chip8Core *core = (Chip8Core *)userdata;
    int16_t mono[SAMPLES_PER_TICK];
    chip8_audio_render_samples(&core->oscillator, mono, SAMPLES_PER_TICK, machine->sound_timer > 0);
    ring_push(core, mono, SAMPLES_PER_TICK);
}

static int probe(const uint8_t *head, size_t head_size, size_t size, const char *path) {
    (void)head;
    (void)head_size;
    if (size == 0 || size > MAX_PROGRAM) return 0;
    if (path) {
        const char *dot = strrchr(path, '.');
        if (dot && (!strcmp(dot, ".ch8") || !strcmp(dot, ".c8") || !strcmp(dot, ".CH8") ||
                    !strcmp(dot, ".C8")))
            return 60;
    }
    /* CHIP-8 programs carry no signature, so anything small enough is a maybe. */
    return 10;
}

static void *create(const NacreHost *host, char *error, size_t error_capacity) {
    Chip8Core *core = (Chip8Core *)calloc(1, sizeof(Chip8Core));
    if (!core) {
        snprintf(error, error_capacity, "out of memory");
        return NULL;
    }
    if (host) core->host = *host;
    core->cpu_hz = CHIP8_DEFAULT_CPU_HZ;
    core->speed = 100;
    chip8_init(&core->machine, CHIP8_SEED);
    chip8_scheduler_init(&core->scheduler, core->cpu_hz);
    chip8_audio_oscillator_reset(&core->oscillator);
    core->frame.format = NACRE_PIXEL_INDEX8;
    core->frame.width = CHIP8_DISPLAY_WIDTH;
    core->frame.height = CHIP8_DISPLAY_HEIGHT;
    core->frame.pitch = CHIP8_DISPLAY_WIDTH;
    core->frame.pixels = core->shown;
    core->frame.levels = 2;
    core->frame.aspect = 0.0f;
    return core;
}

static void destroy(void *self) { free(self); }

static void audio_clear(void *self) {
    Chip8Core *core = (Chip8Core *)self;
    core->ring_read = 0;
    core->ring_count = 0;
}

/* Fresh machine with the stored program. */
static int boot(Chip8Core *core, char *error, size_t error_capacity) {
    Chip8 machine;
    chip8_init(&machine, CHIP8_SEED);
    if (!chip8_load_bytes(&machine, core->program, core->program_size, error, error_capacity))
        return 0;
    core->machine = machine;
    chip8_scheduler_init(&core->scheduler, core->cpu_hz);
    chip8_audio_oscillator_reset(&core->oscillator);
    audio_clear(core);
    core->faulted = 0;
    core->error[0] = '\0';
    core->speed_carry = 0;
    return 1;
}

static int load(void *self, const NacreContent *content, char *error, size_t error_capacity) {
    Chip8Core *core = (Chip8Core *)self;
    if (!content || !content->data) {
        snprintf(error, error_capacity, "CHIP-8 programs are loaded from memory");
        return 0;
    }
    if (content->size > MAX_PROGRAM) {
        snprintf(error, error_capacity, "a CHIP-8 program can be at most %u bytes",
                 (unsigned)MAX_PROGRAM);
        return 0;
    }
    memcpy(core->program, content->data, content->size);
    core->program_size = content->size;
    core->program_hash = fnv1a(content->data, content->size);
    if (!boot(core, error, error_capacity)) return 0;
    core->loaded = 1;
    return 1;
}

static void reset(void *self) {
    Chip8Core *core = (Chip8Core *)self;
    char ignored[64];
    if (core->loaded) boot(core, ignored, sizeof ignored);
}

static int advance(void *self, uint64_t elapsed_ns) {
    Chip8Core *core = (Chip8Core *)self;
    if (!core->loaded || core->faulted) return 1;
    if (!core->running) {
        /* Same as a paused frame before: drop partial instruction progress. */
        chip8_scheduler_advance(&core->machine, &core->scheduler, 0, 1, NULL, NULL, NULL, 0);
        core->speed_carry = 0;
        return 1;
    }
    if (elapsed_ns > CHIP8_MAX_FRAME_DELTA_NS) elapsed_ns = CHIP8_MAX_FRAME_DELTA_NS;
    uint64_t scaled = elapsed_ns * core->speed + core->speed_carry;
    core->speed_carry = scaled % 100u;
    if (!chip8_scheduler_advance(&core->machine, &core->scheduler, scaled / 100u, 0, on_timer, core,
                                 core->error, sizeof core->error)) {
        core->faulted = 1;
        return 0;
    }
    return 1;
}

static void set_running(void *self, int running) { ((Chip8Core *)self)->running = running != 0; }

static void set_speed(void *self, unsigned percent) {
    Chip8Core *core = (Chip8Core *)self;
    core->speed = percent ? percent : 100;
    core->speed_carry = 0;
}

static const char *error_text(void *self) {
    Chip8Core *core = (Chip8Core *)self;
    return core->faulted ? core->error : NULL;
}

static const NacreVideoFrame *video(void *self) {
    Chip8Core *core = (Chip8Core *)self;
    uint8_t now[PIXELS];
    if (!core->loaded) return NULL;
    for (size_t i = 0; i < PIXELS; i++) now[i] = core->machine.display[i] ? 1 : 0;
    if (memcmp(now, core->shown, PIXELS) != 0) {
        memcpy(core->shown, now, PIXELS);
        core->frame.frame_count++;
    }
    return &core->frame;
}

static unsigned audio_rate(void *self) {
    (void)self;
    return CHIP8_AUDIO_SAMPLE_RATE;
}

static size_t audio_read(void *self, int16_t *stereo, size_t max_frames) {
    Chip8Core *core = (Chip8Core *)self;
    size_t n = core->ring_count < max_frames ? core->ring_count : max_frames;
    for (size_t i = 0; i < n; i++) {
        size_t at = (core->ring_read + i) % RING_FRAMES;
        stereo[i * 2] = core->ring[at * 2];
        stereo[i * 2 + 1] = core->ring[at * 2 + 1];
    }
    core->ring_read = (core->ring_read + n) % RING_FRAMES;
    core->ring_count -= n;
    return n;
}

static void set_keypad(void *self, unsigned key, int pressed) {
    Chip8Core *core = (Chip8Core *)self;
    if (key < 16) chip8_set_key(&core->machine, (uint8_t)key, pressed);
}

static size_t state_size(void *self) {
    Chip8Core *core = (Chip8Core *)self;
    return core->loaded ? sizeof(StateHeader) + sizeof(Chip8) + sizeof(Chip8Scheduler) : 0;
}

static size_t state_save(void *self, uint8_t *out, size_t capacity) {
    Chip8Core *core = (Chip8Core *)self;
    size_t size = state_size(self);
    StateHeader header;
    if (!size || capacity < size) return 0;
    header.magic = STATE_MAGIC;
    header.version = STATE_VERSION;
    header.machine_size = (uint32_t)sizeof(Chip8);
    header.program_hash = core->program_hash;
    memcpy(out, &header, sizeof header);
    memcpy(out + sizeof header, &core->machine, sizeof(Chip8));
    memcpy(out + sizeof header + sizeof(Chip8), &core->scheduler, sizeof(Chip8Scheduler));
    return size;
}

static int state_load(void *self, const uint8_t *data, size_t size) {
    Chip8Core *core = (Chip8Core *)self;
    StateHeader header;
    Chip8Scheduler scheduler;
    uint8_t held[16];
    if (!core->loaded || !data || size != state_size(self)) return 0;
    memcpy(&header, data, sizeof header);
    if (header.magic != STATE_MAGIC || header.version != STATE_VERSION ||
        header.machine_size != sizeof(Chip8) || header.program_hash != core->program_hash)
        return 0;
    memcpy(&scheduler, data + sizeof header + sizeof(Chip8), sizeof scheduler);
    /* Keys follow the player's hands, not the snapshot. */
    memcpy(held, core->machine.keys, sizeof held);
    memcpy(&core->machine, data + sizeof header, sizeof(Chip8));
    memcpy(core->machine.keys, held, sizeof held);
    core->machine.key_press_edges = 0;
    core->machine.key_release_edges = 0;
    /* The speed setting belongs to the user, not to the snapshot. */
    core->scheduler = scheduler;
    chip8_scheduler_set_cpu_hz(&core->scheduler, core->cpu_hz);
    core->faulted = 0;
    core->error[0] = '\0';
    audio_clear(core);
    return 1;
}

static int set_option(void *self, const char *key, const char *value) {
    Chip8Core *core = (Chip8Core *)self;
    if (key && value && !strcmp(key, "cpu_hz")) {
        char *end;
        unsigned long hz = strtoul(value, &end, 10);
        if (end == value || *end || hz == 0 || hz > 60000ul) return 0;
        core->cpu_hz = (uint32_t)hz;
        chip8_scheduler_set_cpu_hz(&core->scheduler, core->cpu_hz);
        return 1;
    }
    return 0;
}

static const char *title(void *self) {
    (void)self;
    return NULL;
}

const NacreCore nacre_chip8_core = {
    .api_version = NACRE_CORE_API_VERSION,
    .id = "chip8",
    .name = "CHIP-8",
    .extensions = "ch8;c8",
    .flags = NACRE_CORE_HAS_STATES,
    .keypad_keys = 16,
    .probe = probe,
    .create = create,
    .destroy = destroy,
    .load = load,
    .reset = reset,
    .advance = advance,
    .set_running = set_running,
    .set_speed = set_speed,
    .error = error_text,
    .video = video,
    .audio_rate = audio_rate,
    .audio_read = audio_read,
    .audio_clear = audio_clear,
    .set_keypad = set_keypad,
    .state_size = state_size,
    .state_save = state_save,
    .state_load = state_load,
    .set_option = set_option,
    .title = title,
};
