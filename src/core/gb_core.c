/* The Game Boy behind the core interface: a thin wrapper over gb.h. */

#include "nacre_core.h"

#include "gb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    NacreHost host;
    Gb *gb;
    int loaded;
    int running;
    int faulted;
    unsigned speed;
    uint32_t buttons; /* NACRE_PAD_BIT set last passed to the machine */
    char error[256];
    NacreVideoFrame frame;
} GbCore;

/* Pad button behind each GbButton, in GbButton order. */
static const NacrePadButton BUTTON_FOR[GB_BUTTON_COUNT] = {
    NACRE_PAD_RIGHT, NACRE_PAD_LEFT, NACRE_PAD_UP, NACRE_PAD_DOWN,
    NACRE_PAD_EAST,  NACRE_PAD_SOUTH, NACRE_PAD_SELECT, NACRE_PAD_START,
};

static int probe(const uint8_t *head, size_t head_size, size_t size, const char *path) {
    (void)size;
    (void)path;
    /* The boot logo is required by real hardware, so it is a reliable signature. */
    return gb_rom_looks_valid(head, head_size) ? 100 : 0;
}

static void *create(const NacreHost *host, char *error, size_t error_capacity) {
    GbCore *core = (GbCore *)calloc(1, sizeof(GbCore));
    if (core) core->gb = gb_create();
    if (!core || !core->gb) {
        free(core);
        snprintf(error, error_capacity, "out of memory");
        return NULL;
    }
    if (host) core->host = *host;
    core->speed = 100;
    core->frame.format = NACRE_PIXEL_INDEX8;
    core->frame.width = GB_SCREEN_WIDTH;
    core->frame.height = GB_SCREEN_HEIGHT;
    core->frame.pitch = GB_SCREEN_WIDTH;
    core->frame.levels = 4;
    return core;
}

static void destroy(void *self) {
    GbCore *core = (GbCore *)self;
    if (!core) return;
    gb_destroy(core->gb);
    free(core);
}

static int load(void *self, const NacreContent *content, char *error, size_t error_capacity) {
    GbCore *core = (GbCore *)self;
    if (!content || !content->data) {
        snprintf(error, error_capacity, "Game Boy ROMs are loaded from memory");
        return 0;
    }
    if (!gb_load_rom(core->gb, content->data, content->size, error, error_capacity)) return 0;
    core->loaded = 1;
    return 1;
}

static void reset(void *self) {
    GbCore *core = (GbCore *)self;
    if (!core->loaded) return;
    gb_reset(core->gb);
    gb_audio_clear(core->gb);
    core->faulted = 0;
    core->error[0] = '\0';
}

static int advance(void *self, uint64_t elapsed_ns) {
    GbCore *core = (GbCore *)self;
    if (!core->loaded || core->faulted || !core->running) return 1;
    if (!gb_advance_ns(core->gb, elapsed_ns, core->speed, core->error, sizeof core->error)) {
        core->faulted = 1;
        return 0;
    }
    return 1;
}

static void set_running(void *self, int running) { ((GbCore *)self)->running = running != 0; }

static void set_speed(void *self, unsigned percent) { ((GbCore *)self)->speed = percent ? percent : 100; }

static const char *error_text(void *self) {
    GbCore *core = (GbCore *)self;
    return core->faulted ? core->error : NULL;
}

static const NacreVideoFrame *video(void *self) {
    GbCore *core = (GbCore *)self;
    if (!core->loaded) return NULL;
    core->frame.pixels = gb_framebuffer(core->gb);
    core->frame.frame_count = gb_frame_count(core->gb);
    return &core->frame;
}

static unsigned audio_rate(void *self) {
    (void)self;
    return GB_AUDIO_SAMPLE_RATE;
}

static size_t audio_read(void *self, int16_t *stereo, size_t max_frames) {
    return gb_audio_read(((GbCore *)self)->gb, stereo, max_frames);
}

static void audio_clear(void *self) { gb_audio_clear(((GbCore *)self)->gb); }

static void set_pad(void *self, unsigned port, uint32_t buttons) {
    GbCore *core = (GbCore *)self;
    if (port != 0) return;
    for (int b = 0; b < GB_BUTTON_COUNT; b++) {
        uint32_t bit = NACRE_PAD_BIT(BUTTON_FOR[b]);
        if ((buttons ^ core->buttons) & bit) gb_set_button(core->gb, (GbButton)b, (buttons & bit) != 0);
    }
    core->buttons = buttons;
}

static size_t save_size(void *self) {
    GbCore *core = (GbCore *)self;
    return core->loaded && gb_has_battery(core->gb) ? gb_save_size(core->gb) : 0;
}

static size_t save_write(void *self, uint8_t *out, size_t capacity) {
    return gb_save_write(((GbCore *)self)->gb, out, capacity);
}

static int save_load(void *self, const uint8_t *data, size_t size) {
    return gb_save_load(((GbCore *)self)->gb, data, size);
}

static int save_dirty(void *self) { return gb_save_dirty(((GbCore *)self)->gb); }

static void save_mark_clean(void *self) { gb_save_mark_clean(((GbCore *)self)->gb); }

static void set_wall_clock(void *self, int64_t unix_seconds) {
    gb_set_wall_clock(((GbCore *)self)->gb, unix_seconds);
}

static size_t state_size(void *self) { return gb_state_size(((GbCore *)self)->gb); }

static size_t state_save(void *self, uint8_t *out, size_t capacity) {
    return gb_state_save(((GbCore *)self)->gb, out, capacity);
}

static int state_load(void *self, const uint8_t *data, size_t size) {
    GbCore *core = (GbCore *)self;
    if (!gb_state_load(core->gb, data, size)) return 0;
    /* Buttons follow the player's hands, not the snapshot. */
    for (int b = 0; b < GB_BUTTON_COUNT; b++)
        gb_set_button(core->gb, (GbButton)b, (core->buttons & NACRE_PAD_BIT(BUTTON_FOR[b])) != 0);
    core->faulted = 0;
    core->error[0] = '\0';
    return 1;
}

static const char *title(void *self) {
    GbCore *core = (GbCore *)self;
    const char *text = core->loaded ? gb_title(core->gb) : NULL;
    return text && *text ? text : NULL;
}

const NacreCore nacre_gb_core = {
    .api_version = NACRE_CORE_API_VERSION,
    .id = "gb",
    .name = "Game Boy",
    .extensions = "gb;dmg",
    .flags = NACRE_CORE_HAS_STATES | NACRE_CORE_HAS_BATTERY,
    .pad_labels = {
        [NACRE_PAD_UP] = "Up",
        [NACRE_PAD_DOWN] = "Down",
        [NACRE_PAD_LEFT] = "Left",
        [NACRE_PAD_RIGHT] = "Right",
        [NACRE_PAD_EAST] = "A",
        [NACRE_PAD_SOUTH] = "B",
        [NACRE_PAD_SELECT] = "Select",
        [NACRE_PAD_START] = "Start",
    },
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
    .set_pad = set_pad,
    .save_size = save_size,
    .save_write = save_write,
    .save_load = save_load,
    .save_dirty = save_dirty,
    .save_mark_clean = save_mark_clean,
    .set_wall_clock = set_wall_clock,
    .state_size = state_size,
    .state_save = state_save,
    .state_load = state_load,
    .title = title,
};
