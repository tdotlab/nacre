/* The Nintendo DS behind the core interface: a thin wrapper over nds.h. */

#include "nacre_core.h"

#include "nds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    NacreHost host;
    Nds *nds;
    int loaded;
    int running;
    int faulted;
    unsigned speed;
    uint32_t buttons; /* NACRE_PAD_BIT set last passed to the machine */
    char error[256];
    NacreVideoFrame frame;
} NdsCore;

/* Pad button behind each NdsButton, in NdsButton order. */
static const NacrePadButton BUTTON_FOR[NDS_BUTTON_COUNT] = {
    NACRE_PAD_EAST,  NACRE_PAD_SOUTH, NACRE_PAD_SELECT, NACRE_PAD_START,
    NACRE_PAD_RIGHT, NACRE_PAD_LEFT,  NACRE_PAD_UP,     NACRE_PAD_DOWN,
    NACRE_PAD_R1,    NACRE_PAD_L1,    NACRE_PAD_NORTH,  NACRE_PAD_WEST,
};

static int probe(const uint8_t *head, size_t head_size, size_t size, const char *path) {
    (void)size;
    (void)path;
    return nds_rom_looks_valid(head, head_size) ? 100 : 0;
}

static void *create(const NacreHost *host, char *error, size_t error_capacity) {
    NdsCore *core = (NdsCore *)calloc(1, sizeof(NdsCore));
    if (core) core->nds = nds_create();
    if (!core || !core->nds) {
        free(core);
        snprintf(error, error_capacity, "out of memory");
        return NULL;
    }
    if (host) core->host = *host;
    core->speed = 100;
    core->frame.format = NACRE_PIXEL_XRGB8888;
    core->frame.width = NDS_FRAME_WIDTH;
    core->frame.height = NDS_FRAME_HEIGHT;
    core->frame.pitch = NDS_FRAME_WIDTH * sizeof(uint32_t);
    return core;
}

static void destroy(void *self) {
    NdsCore *core = (NdsCore *)self;
    if (!core) return;
    nds_destroy(core->nds);
    free(core);
}

static int load(void *self, const NacreContent *content, char *error, size_t error_capacity) {
    NdsCore *core = (NdsCore *)self;
    if (!content || !content->data) {
        snprintf(error, error_capacity, "DS cartridges are loaded from memory");
        return 0;
    }
    if (!nds_load_rom(core->nds, content->data, content->size, error, error_capacity)) return 0;
    core->loaded = 1;
    return 1;
}

static void reset(void *self) {
    NdsCore *core = (NdsCore *)self;
    if (!core->loaded) return;
    nds_reset(core->nds);
    nds_audio_clear(core->nds);
    core->faulted = 0;
    core->error[0] = '\0';
}

static int advance(void *self, uint64_t elapsed_ns) {
    NdsCore *core = (NdsCore *)self;
    if (!core->loaded || core->faulted || !core->running) return 1;
    if (!nds_advance_ns(core->nds, elapsed_ns, core->speed, core->error, sizeof core->error)) {
        core->faulted = 1;
        return 0;
    }
    return 1;
}

static void set_running(void *self, int running) { ((NdsCore *)self)->running = running != 0; }

static void set_speed(void *self, unsigned percent) { ((NdsCore *)self)->speed = percent ? percent : 100; }

static const char *error_text(void *self) {
    NdsCore *core = (NdsCore *)self;
    return core->faulted ? core->error : NULL;
}

static const NacreVideoFrame *video(void *self) {
    NdsCore *core = (NdsCore *)self;
    if (!core->loaded) return NULL;
    core->frame.pixels = nds_framebuffer(core->nds);
    core->frame.frame_count = nds_frame_count(core->nds);
    return &core->frame;
}

static unsigned audio_rate(void *self) {
    (void)self;
    return NDS_AUDIO_SAMPLE_RATE;
}

static size_t audio_read(void *self, int16_t *stereo, size_t max_frames) {
    return nds_audio_read(((NdsCore *)self)->nds, stereo, max_frames);
}

static void audio_clear(void *self) { nds_audio_clear(((NdsCore *)self)->nds); }

static void set_pad(void *self, unsigned port, uint32_t buttons) {
    NdsCore *core = (NdsCore *)self;
    if (port != 0) return;
    for (int b = 0; b < NDS_BUTTON_COUNT; b++) {
        uint32_t bit = NACRE_PAD_BIT(BUTTON_FOR[b]);
        if ((buttons ^ core->buttons) & bit) nds_set_button(core->nds, (NdsButton)b, (buttons & bit) != 0);
    }
    core->buttons = buttons;
}

/* The frame is the top screen over the touchscreen. */
static void set_pointer(void *self, unsigned port, int x, int y, int pressed) {
    NdsCore *core = (NdsCore *)self;
    if (port != 0) return;
    if (y >= NDS_SCREEN_HEIGHT)
        nds_set_touch(core->nds, x, y - NDS_SCREEN_HEIGHT, pressed);
    else
        nds_set_touch(core->nds, -1, -1, 0);
}

/* Days from 1970-01-01 to a proleptic Gregorian date. */
static int64_t days_from_civil(int64_t y, int m, int d) {
    int64_t era, yoe, doy;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* The DS clock keeps local time, so the host's UTC seconds are shifted by
 * the local offset in effect at that moment. */
static void set_wall_clock(void *self, int64_t unix_seconds) {
    time_t t = (time_t)unix_seconds;
    struct tm *local = localtime(&t);
    int64_t seconds = unix_seconds;
    if (local)
        seconds = days_from_civil(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday) * 86400 +
                  local->tm_hour * 3600 + local->tm_min * 60 + local->tm_sec;
    nds_set_wall_clock(((NdsCore *)self)->nds, seconds);
}

static size_t save_size(void *self) {
    NdsCore *core = (NdsCore *)self;
    return core->loaded ? nds_save_size(core->nds) : 0;
}

static size_t save_write(void *self, uint8_t *out, size_t capacity) {
    return nds_save_write(((NdsCore *)self)->nds, out, capacity);
}

static int save_load(void *self, const uint8_t *data, size_t size) {
    return nds_save_load(((NdsCore *)self)->nds, data, size);
}

static int save_dirty(void *self) { return nds_save_dirty(((NdsCore *)self)->nds); }

static void save_mark_clean(void *self) { nds_save_mark_clean(((NdsCore *)self)->nds); }

/* save_type: auto, none, eeprom512, eeprom8k, eeprom64k, eeprom128k,
 * fram32k, flash256k, flash512k or flash1m. */
static int set_option(void *self, const char *key, const char *value) {
    NdsCore *core = (NdsCore *)self;
    NdsSaveType type;
    if (!key || strcmp(key, "save_type")) return 0;
    type = nds_save_type_from_name(value);
    if (type == NDS_SAVE_TYPE_COUNT) return 0;
    nds_set_save_type(core->nds, type);
    return 1;
}

static const char *title(void *self) {
    NdsCore *core = (NdsCore *)self;
    const char *text = core->loaded ? nds_title(core->nds) : NULL;
    return text && *text ? text : NULL;
}

const NacreCore nacre_nds_core = {
    .api_version = NACRE_CORE_API_VERSION,
    .id = "nds",
    .name = "Nintendo DS",
    .extensions = "nds",
    .flags = NACRE_CORE_HAS_POINTER | NACRE_CORE_HAS_BATTERY | NACRE_CORE_SAVE_SIZED_LATER,
    .pad_labels = {
        [NACRE_PAD_UP] = "Up",
        [NACRE_PAD_DOWN] = "Down",
        [NACRE_PAD_LEFT] = "Left",
        [NACRE_PAD_RIGHT] = "Right",
        [NACRE_PAD_EAST] = "A",
        [NACRE_PAD_SOUTH] = "B",
        [NACRE_PAD_NORTH] = "X",
        [NACRE_PAD_WEST] = "Y",
        [NACRE_PAD_L1] = "L",
        [NACRE_PAD_R1] = "R",
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
    .set_pointer = set_pointer,
    .save_size = save_size,
    .save_write = save_write,
    .save_load = save_load,
    .save_dirty = save_dirty,
    .save_mark_clean = save_mark_clean,
    .set_wall_clock = set_wall_clock,
    .set_option = set_option,
    .title = title,
};
