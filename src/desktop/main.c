#include "audio.h"
#include "chip8.h"
#include "gb.h"
#include "gb_input.h"
#include "pad.h"
#include "scheduler.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WINDOW_WIDTH 960
#define WINDOW_HEIGHT 480
#define AUDIO_CALLBACK_SAMPLES 1024
#define MAX_CHIP8_ROM (CHIP8_MEMORY_SIZE - CHIP8_PROGRAM_START)
#define MAX_GB_ROM (8u << 20) /* the largest MBC5 cartridge */
#define MAX_SAVE (1u << 20)
#define MAX_STATE (1u << 24)
#define GB_FAST_PERCENT 400u /* while fast-forward is held */
#define GB_STATE_SLOTS 4

enum {
    FILE_EVENT_OPENED = 1,
    FILE_EVENT_ERROR = 2
};

typedef struct {
    SDL_AudioStream *stream;
    SDL_AtomicInt active;
    Chip8AudioOscillator oscillator;
} DesktopAudio;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *framebuffer;
    SDL_Texture *gb_framebuffer;
    bool sdl_initialized;
    Uint32 rom_event_type;
    SDL_AtomicInt dialog_pending;
    DesktopAudio audio;
    Chip8 machine;
    Chip8Scheduler scheduler;
    char *rom_path;
    uint32_t seed;
    bool audio_available;
    bool muted;
    bool user_paused;
    bool focus_paused;
    bool quit_requested;

    /* Game Boy session; `is_gb` says which machine is current. */
    Gb *gb;
    bool is_gb;
    int gb_speed;   /* index into gb_speed_percent */
    int gb_palette; /* index into gb_palette_light */
    SDL_AudioStream *gb_stream;
    bool gb_stream_running;
    char *save_path; /* NULL when the cartridge has no battery */
    uint64_t save_due;

    /* Game Boy input: rebindable keys, a controller, hold-to-fast-forward. */
    GbBindings keys;
    bool kb_held[GB_BUTTON_COUNT]; /* buttons the keyboard holds */
    unsigned pad_held, pad_seen;   /* controller actions now / last poll */
    bool ff_key, ff_on;
    int slot; /* save-state slot, 0..GB_STATE_SLOTS-1 */
    char status[96];
    uint64_t status_until;
    char *prefs_path; /* controls.txt in the user's preference folder */

    /* The controls overlay (F10). */
    bool rebinding;
    bool capturing; /* waiting for the key to bind */
    int rb_action, rb_slot;
    char rb_note[96];
} App;

static const SDL_DialogFileFilter rom_filters[] = {
    { "Game ROMs", "ch8;gb;gbc;rom;bin" },
    { "CHIP-8 ROMs", "ch8;rom;bin" },
    { "Game Boy ROMs", "gb;gbc" },
    { "All files", "*" }
};

static const unsigned gb_speed_percent[3] = { 50, 100, 200 };
static const char *const gb_speed_name[3] = { "0.5x", "1x", "2x" };
/* The desktop apps' Fern, Amber, and Pearl inks, shaded down to a dark green. */
static const uint32_t gb_palette_light[3] = { 0xC3DCC7, 0xEBD8AF, 0xE8E9DF };
static const char *const gb_palette_name[3] = { "Fern", "Amber", "Pearl" };
#define GB_PALETTE_DARK 0x1E2A25u

/* Arrows and WASD for the D-pad, X/K for A, Z/J for B, Enter for Start, Backspace or
 * right Shift for Select; Tab fast-forwards, F5 and F7 save and load, F6 changes slot. */
static const int default_keys[GBI_COUNT][GBI_KEYS_PER_ACTION] = {
    { SDL_SCANCODE_RIGHT, SDL_SCANCODE_D },
    { SDL_SCANCODE_LEFT, SDL_SCANCODE_A },
    { SDL_SCANCODE_UP, SDL_SCANCODE_W },
    { SDL_SCANCODE_DOWN, SDL_SCANCODE_S },
    { SDL_SCANCODE_X, SDL_SCANCODE_K },
    { SDL_SCANCODE_Z, SDL_SCANCODE_J },
    { SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RSHIFT },
    { SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER },
    { SDL_SCANCODE_TAB, GBI_UNBOUND },
    { SDL_SCANCODE_F5, GBI_UNBOUND },
    { SDL_SCANCODE_F7, GBI_UNBOUND },
    { SDL_SCANCODE_F6, GBI_UNBOUND },
};

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                                   int additional_amount, int total_amount) {
    (void)total_amount;
    DesktopAudio *audio = (DesktopAudio *)userdata;
    int remaining = (additional_amount + (int)sizeof(int16_t) - 1) /
                    (int)sizeof(int16_t);
    int16_t samples[AUDIO_CALLBACK_SAMPLES];

    while (remaining > 0) {
        int count = remaining < AUDIO_CALLBACK_SAMPLES
            ? remaining : AUDIO_CALLBACK_SAMPLES;
        int active = SDL_GetAtomicInt(&audio->active) != 0;
        chip8_audio_render_samples(&audio->oscillator, samples,
                                   (size_t)count, active);
        if (!SDL_PutAudioStreamData(stream, samples,
                                    count * (int)sizeof(samples[0]))) {
            return;
        }
        remaining -= count;
    }
}

static int is_paused(const App *app) {
    return app->user_paused || app->focus_paused || app->rebinding;
}

static void sync_audio(App *app) {
    int active = !app->is_gb && app->audio_available && !app->muted &&
                 !is_paused(app) && app->machine.sound_timer > 0;
    SDL_SetAtomicInt(&app->audio.active, active);

    bool gb_run = app->is_gb && app->gb_stream && !app->muted && !is_paused(app);
    if (app->gb_stream && gb_run != app->gb_stream_running) {
        SDL_ClearAudioStream(app->gb_stream);
        if (gb_run) {
            gb_audio_clear(app->gb);
            SDL_ResumeAudioStreamDevice(app->gb_stream);
        } else {
            SDL_PauseAudioStreamDevice(app->gb_stream);
        }
        app->gb_stream_running = gb_run;
    }
}

/* Moves this frame's Game Boy samples to the device. The emulator follows the
 * wall clock and the device its own crystal, so the queue is held between about
 * 40 and 150 ms: silence pads an underrun and a long queue drops a batch. */
static void pump_gb_audio(App *app) {
    static int16_t samples[4096 * 2];
    static const int16_t silence[480 * 2] = { 0 };
    if (!app->gb) return;
    if (!app->gb_stream_running) {
        gb_audio_clear(app->gb);
        return;
    }
    if (app->ff_on) { /* four times faster is a squeal, not music: run silent, let the queue drain */
        while (gb_audio_read(app->gb, samples, 4096) > 0) {}
        return;
    }
    float ratio = (float)gb_speed_percent[app->gb_speed] / 100.0f;
    SDL_SetAudioStreamFrequencyRatio(app->gb_stream, ratio);
    const int bytes_per_ms = (int)(GB_AUDIO_SAMPLE_RATE / 1000u) * 4;
    int queued = SDL_GetAudioStreamQueued(app->gb_stream);
    bool drop = queued > (int)(150.0f * (float)bytes_per_ms * ratio);
    if (queued >= 0 && queued < (int)(20.0f * (float)bytes_per_ms * ratio)) {
        int pad = (int)(40.0f * (float)bytes_per_ms * ratio) - queued;
        while (pad >= 4) {
            int chunk = pad < (int)sizeof(silence) ? (pad & ~3) : (int)sizeof(silence);
            SDL_PutAudioStreamData(app->gb_stream, silence, chunk);
            pad -= chunk;
        }
    }
    size_t frames;
    while ((frames = gb_audio_read(app->gb, samples, 4096)) > 0) {
        if (!drop) SDL_PutAudioStreamData(app->gb_stream, samples, (int)(frames * 4));
    }
}

static void timer_tick(Chip8 *machine, void *userdata) {
    App *app = (App *)userdata;
    (void)machine;
    sync_audio(app);
}

static void update_title(App *app) {
    char title[160];
    if (app->is_gb) {
        const char *cart = gb_title(app->gb);
        snprintf(title, sizeof(title), "Nacre - %s (Game Boy, %s, %s)%s",
                 cart[0] ? cart : "untitled", gb_speed_name[app->gb_speed],
                 gb_palette_name[app->gb_palette], app->user_paused ? " - paused" : "");
    } else {
        snprintf(title, sizeof(title), "Nacre%s", app->user_paused ? " - paused" : "");
    }
    SDL_SetWindowTitle(app->window, title);
}

/* Reads a whole file of 1..limit bytes into a new SDL_malloc buffer. */
static uint8_t *read_file(const char *path, size_t limit, size_t *length,
                          char *error, size_t error_capacity) {
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (!file) {
        snprintf(error, error_capacity, "cannot open '%s': %s", path, SDL_GetError());
        return NULL;
    }
    Sint64 file_size = SDL_GetIOSize(file);
    if (file_size <= 0 || (uint64_t)file_size > limit) {
        SDL_CloseIO(file);
        snprintf(error, error_capacity, "'%s' is empty, too large, or cannot be measured", path);
        return NULL;
    }
    uint8_t *bytes = (uint8_t *)SDL_malloc((size_t)file_size);
    if (!bytes) {
        SDL_CloseIO(file);
        snprintf(error, error_capacity, "not enough memory to read '%s'", path);
        return NULL;
    }
    if (SDL_ReadIO(file, bytes, (size_t)file_size) != (size_t)file_size) {
        SDL_CloseIO(file);
        SDL_free(bytes);
        snprintf(error, error_capacity, "could not read '%s'", path);
        return NULL;
    }
    SDL_CloseIO(file);
    *length = (size_t)file_size;
    return bytes;
}

/* "Tetris.gb" with ".sav" is "Tetris.sav" in the same folder; ".state2" is a save-state slot. */
static char *sibling_path(const char *rom_path, const char *extension) {
    const char *slash = strrchr(rom_path, '/');
    const char *dot = strrchr(rom_path, '.');
    size_t stem = (dot && (!slash || dot > slash + 1)) ? (size_t)(dot - rom_path) : strlen(rom_path);
    char *path = NULL;
    if (SDL_asprintf(&path, "%.*s%s", (int)stem, rom_path, extension) < 0) return NULL;
    return path;
}

/* Writes a temporary file, then renames it over the old one so an interrupted
 * write never leaves a torn file. */
static bool write_atomic(const char *path, const void *data, size_t size) {
    char *temp = NULL;
    bool ok = SDL_asprintf(&temp, "%s.tmp", path) >= 0;
    if (ok) {
        SDL_IOStream *file = SDL_IOFromFile(temp, "wb");
        ok = file && SDL_WriteIO(file, data, size) == size;
        if (file) ok = SDL_CloseIO(file) && ok;
        if (ok) ok = SDL_RenamePath(temp, path);
        if (!ok) SDL_RemovePath(temp);
    }
    SDL_free(temp);
    return ok;
}

/* Writes changed cartridge RAM. */
static void write_save(App *app) {
    app->save_due = 0;
    if (!app->is_gb || !app->save_path || !gb_save_dirty(app->gb)) return;
    gb_set_wall_clock(app->gb, (int64_t)time(NULL));
    size_t size = gb_save_size(app->gb);
    uint8_t *data = size ? (uint8_t *)SDL_malloc(size) : NULL;
    bool ok = false;
    if (data) {
        size = gb_save_write(app->gb, data, size);
        ok = write_atomic(app->save_path, data, size);
    }
    if (ok) gb_save_mark_clean(app->gb);
    else fprintf(stderr, "could not write the save '%s': %s\n", app->save_path, SDL_GetError());
    SDL_free(data);
}

static void read_save(App *app) {
    char error[256];
    size_t length = 0;
    gb_set_wall_clock(app->gb, (int64_t)time(NULL));
    if (!SDL_GetPathInfo(app->save_path, NULL)) return; /* no save yet */
    uint8_t *data = read_file(app->save_path, MAX_SAVE, &length, error, sizeof(error));
    if (data && gb_save_load(app->gb, data, length)) {
        fprintf(stderr, "loaded the save '%s'\n", app->save_path);
    } else {
        /* Leave a save that doesn't fit this cartridge alone rather than overwrite it. */
        fprintf(stderr, "ignoring the save '%s'; it doesn't fit this cartridge\n",
                app->save_path);
        SDL_free(app->save_path);
        app->save_path = NULL;
    }
    SDL_free(data);
}

/* A file with a Game Boy cartridge header runs on the Game Boy core; anything
 * else must be a CHIP-8 program. A failed load keeps the current session. */
static int load_rom(App *app, const char *path, char *error,
                    size_t error_capacity) {
    char *owned_path = SDL_strdup(path);
    if (!owned_path) {
        snprintf(error, error_capacity, "not enough memory to remember the ROM path");
        return 0;
    }
    size_t length = 0;
    uint8_t *bytes = read_file(path, MAX_GB_ROM, &length, error, error_capacity);
    if (!bytes) {
        SDL_free(owned_path);
        return 0;
    }

    bool gb_rom = gb_rom_looks_valid(bytes, length) != 0;
    Chip8 candidate;
    if (gb_rom) {
        if (!app->gb) {
            snprintf(error, error_capacity, "the Game Boy core could not start");
            goto fail;
        }
        write_save(app); /* the cartridge being replaced */
        if (!gb_load_rom(app->gb, bytes, length, error, error_capacity)) goto fail;
    } else {
        if (length > MAX_CHIP8_ROM) {
            snprintf(error, error_capacity,
                     "'%s' has no Game Boy cartridge header, and a CHIP-8 ROM can be at "
                     "most %d bytes", path, (int)MAX_CHIP8_ROM);
            goto fail;
        }
        chip8_init(&candidate, app->seed);
        if (!chip8_load_bytes(&candidate, bytes, length, error, error_capacity)) goto fail;
        write_save(app);
        app->machine = candidate;
        chip8_scheduler_init(&app->scheduler, CHIP8_DEFAULT_CPU_HZ);
    }
    SDL_free(bytes);

    SDL_free(app->rom_path);
    app->rom_path = owned_path;
    SDL_free(app->save_path);
    app->save_path = NULL;
    app->is_gb = gb_rom;
    if (gb_rom && gb_has_battery(app->gb)) {
        app->save_path = sibling_path(path, ".sav");
        if (app->save_path) read_save(app);
    }
    app->user_paused = false;
    sync_audio(app);
    update_title(app);
    return 1;

fail:
    SDL_free(bytes);
    SDL_free(owned_path);
    return 0;
}

/* F2 keeps the Game Boy cartridge (and its save RAM) and resets the machine. */
static int restart(App *app, char *error, size_t error_capacity) {
    if (app->is_gb) {
        write_save(app);
        gb_reset(app->gb);
        gb_audio_clear(app->gb);
        app->user_paused = false;
        sync_audio(app);
        update_title(app);
        return 1;
    }
    return load_rom(app, app->rom_path, error, error_capacity);
}

static void show_error(App *app, const char *message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Nacre", message, app->window);
}

static void queue_file_event(App *app, int code, const char *message) {
    SDL_Event event;
    SDL_zero(event);
    event.type = app->rom_event_type;
    event.user.code = code;
    event.user.data1 = message ? SDL_strdup(message) : NULL;
    if (message && !event.user.data1) return;
    if (!SDL_PushEvent(&event)) {
        SDL_free(event.user.data1);
    }
}

static void SDLCALL rom_dialog_callback(void *userdata,
                                        const char * const *filelist,
                                        int filter) {
    (void)filter;
    App *app = (App *)userdata;
    if (!filelist) {
        queue_file_event(app, FILE_EVENT_ERROR, SDL_GetError());
    } else if (filelist[0]) {
        queue_file_event(app, FILE_EVENT_OPENED, filelist[0]);
    }
    SDL_SetAtomicInt(&app->dialog_pending, 0);
}

static void open_rom_dialog(App *app) {
    if (SDL_GetAtomicInt(&app->dialog_pending)) return;
    SDL_SetAtomicInt(&app->dialog_pending, 1);
    SDL_ShowOpenFileDialog(rom_dialog_callback, app, app->window,
                           rom_filters, (int)SDL_arraysize(rom_filters),
                           NULL, false);
}

/* ---- Game Boy input: states, fast-forward, rebinding ---------------------------- */

/* A short message drawn over the game that clears itself after a few seconds. */
static void set_status(App *app, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(app->status, sizeof(app->status), format, args);
    va_end(args);
    app->status_until = SDL_GetTicksNS() + 3000000000ull;
}

static void save_prefs(App *app) {
    char text[512];
    size_t used = gbi_serialize(&app->keys, text, sizeof(text) - 32);
    if (!app->prefs_path || used == 0) return;
    used += (size_t)snprintf(text + used, sizeof(text) - used, "\nslot=%d\n", app->slot);
    if (!write_atomic(app->prefs_path, text, used)) {
        fprintf(stderr, "could not write '%s': %s\n", app->prefs_path, SDL_GetError());
    }
}

static void load_prefs(App *app) {
    char error[128];
    size_t length = 0;
    gbi_set_defaults(&app->keys, default_keys);
    app->slot = 0;
    if (!app->prefs_path || !SDL_GetPathInfo(app->prefs_path, NULL)) return;
    uint8_t *data = read_file(app->prefs_path, 4096, &length, error, sizeof(error));
    if (!data) return;
    char *text = (char *)SDL_malloc(length + 1);
    if (text) {
        memcpy(text, data, length);
        text[length] = '\0';
        char *newline = strchr(text, '\n');
        if (newline) *newline = '\0';
        if (!gbi_parse(&app->keys, text)) {
            fprintf(stderr, "ignoring the controls in '%s'; using the defaults\n", app->prefs_path);
        }
        if (newline) {
            const char *slot = strstr(newline + 1, "slot=");
            if (slot && slot[5] >= '0' && slot[5] < '0' + GB_STATE_SLOTS) app->slot = slot[5] - '0';
        }
        SDL_free(text);
    }
    SDL_free(data);
}

/* "Tetris.gb" keeps its states in "Tetris.state1" .. "Tetris.state4". */
static char *state_path(const App *app, int slot) {
    char extension[16];
    if (!app->rom_path) return NULL;
    snprintf(extension, sizeof(extension), ".state%d", slot + 1);
    return sibling_path(app->rom_path, extension);
}

static void apply_buttons(App *app) {
    if (!app->gb) return;
    for (int button = 0; button < GB_BUTTON_COUNT; button++) {
        bool held = app->kb_held[button] || (app->pad_held & PAD_BIT(button)) != 0;
        gb_set_button(app->gb, (GbButton)button, held);
    }
}

static void save_state(App *app) {
    if (!app->is_gb || !app->gb) return;
    char *path = state_path(app, app->slot);
    size_t size = gb_state_size(app->gb);
    uint8_t *blob = size ? (uint8_t *)SDL_malloc(size) : NULL;
    bool ok = false;
    if (path && blob) {
        size = gb_state_save(app->gb, blob, size);
        ok = size > 0 && write_atomic(path, blob, size);
    }
    if (ok) set_status(app, "Saved state to slot %d", app->slot + 1);
    else set_status(app, "Couldn't save to slot %d", app->slot + 1);
    SDL_free(blob);
    SDL_free(path);
}

static void load_state(App *app) {
    if (!app->is_gb || !app->gb) return;
    char *path = state_path(app, app->slot);
    if (!path || !SDL_GetPathInfo(path, NULL)) {
        set_status(app, "Slot %d is empty", app->slot + 1);
    } else {
        char error[128];
        size_t length = 0;
        uint8_t *blob = read_file(path, MAX_STATE, &length, error, sizeof(error));
        if (blob && gb_state_load(app->gb, blob, length)) {
            /* The state remembers the buttons held when it was saved; what is held now wins. */
            apply_buttons(app);
            gb_audio_clear(app->gb);
            set_status(app, "Loaded state from slot %d", app->slot + 1);
        } else {
            set_status(app, "Slot %d doesn't fit this game", app->slot + 1);
        }
        SDL_free(blob);
    }
    SDL_free(path);
}

static void next_slot(App *app) {
    app->slot = (app->slot + 1) % GB_STATE_SLOTS;
    save_prefs(app);
    set_status(app, "State slot %d", app->slot + 1);
}

static void gb_action_press(App *app, int action, bool repeat) {
    if (action < GBI_BUTTONS) {
        if (!is_paused(app) && !repeat) {
            app->kb_held[action] = true;
            apply_buttons(app);
        }
    } else if (action == GBI_FAST_FORWARD) {
        app->ff_key = true;
    } else if (!repeat) {
        if (action == GBI_SAVE_STATE) save_state(app);
        else if (action == GBI_LOAD_STATE) load_state(app);
        else if (action == GBI_NEXT_SLOT) next_slot(app);
    }
}

static void gb_action_release(App *app, int action) {
    if (action < GBI_BUTTONS) {
        app->kb_held[action] = false;
        apply_buttons(app);
    } else if (action == GBI_FAST_FORWARD) {
        app->ff_key = false;
    }
}

/* Reads the controllers: held buttons feed the console, and the state hotkeys fire once
 * per press. Only the focused, running window listens, so a game in the background isn't played. */
static void poll_pad(App *app) {
    unsigned held = pad_poll();
    const char *note = pad_take_message();
    if (note && app->is_gb) set_status(app, "%s", note);
    if (!app->is_gb || is_paused(app) || SDL_GetAtomicInt(&app->dialog_pending)) held = 0;
    unsigned fresh = held & ~app->pad_seen;
    app->pad_seen = held;
    app->pad_held = held;
    apply_buttons(app);
    if (fresh & PAD_BIT(GBI_SAVE_STATE)) save_state(app);
    if (fresh & PAD_BIT(GBI_LOAD_STATE)) load_state(app);
    if (fresh & PAD_BIT(GBI_NEXT_SLOT)) next_slot(app);
}

/* Keys that already mean something else, so they can't be bound. */
static bool key_reserved(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_SPACE:
        case SDL_SCANCODE_M:
        case SDL_SCANCODE_F2:
        case SDL_SCANCODE_F3:
        case SDL_SCANCODE_F4:
        case SDL_SCANCODE_F10:
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:
        case SDL_SCANCODE_LGUI:
        case SDL_SCANCODE_RGUI:
        case SDL_SCANCODE_APPLICATION:
        case SDL_SCANCODE_UNKNOWN:
            return true;
        default:
            return false;
    }
}

static void key_name(int key, char *out, size_t capacity) {
    const char *name = key == GBI_UNBOUND ? "" : SDL_GetScancodeName((SDL_Scancode)key);
    if (key == GBI_UNBOUND) snprintf(out, capacity, "-");
    else if (name && name[0]) snprintf(out, capacity, "%s", name);
    else snprintf(out, capacity, "key %d", key);
}

static void release_keys(App *app);

static void open_controls(App *app) {
    if (!app->is_gb) {
        set_status(app, "Controls apply to Game Boy games");
        return;
    }
    release_keys(app);
    app->rebinding = true;
    app->capturing = false;
    app->rb_note[0] = '\0';
    sync_audio(app);
}

static void close_controls(App *app) {
    app->rebinding = false;
    app->capturing = false;
    sync_audio(app);
}

/* Navigation inside the overlay: arrows pick the cell, Enter waits for a key,
 * Backspace clears the cell, R restores the defaults. */
static void handle_controls_key(App *app, SDL_Scancode scancode) {
    if (app->capturing) {
        if (scancode == SDL_SCANCODE_ESCAPE) {
            app->capturing = false;
            app->rb_note[0] = '\0';
        } else if (key_reserved(scancode)) {
            snprintf(app->rb_note, sizeof(app->rb_note), "That key is reserved");
        } else {
            int previous = gbi_assign(&app->keys, (GbInput)app->rb_action, app->rb_slot, (int)scancode);
            if (previous >= 0) {
                snprintf(app->rb_note, sizeof(app->rb_note), "Taken from %s", gbi_name((GbInput)previous));
            } else {
                app->rb_note[0] = '\0';
            }
            app->capturing = false;
            save_prefs(app);
        }
        return;
    }
    app->rb_note[0] = '\0';
    switch (scancode) {
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_F10:
            close_controls(app);
            break;
        case SDL_SCANCODE_UP:
            app->rb_action = (app->rb_action + GBI_COUNT - 1) % GBI_COUNT;
            break;
        case SDL_SCANCODE_DOWN:
            app->rb_action = (app->rb_action + 1) % GBI_COUNT;
            break;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_TAB:
            app->rb_slot = (app->rb_slot + 1) % GBI_KEYS_PER_ACTION;
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            app->capturing = true;
            break;
        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_DELETE:
            gbi_assign(&app->keys, (GbInput)app->rb_action, app->rb_slot, GBI_UNBOUND);
            save_prefs(app);
            break;
        case SDL_SCANCODE_R:
            gbi_set_defaults(&app->keys, default_keys);
            save_prefs(app);
            snprintf(app->rb_note, sizeof(app->rb_note), "Defaults restored");
            break;
        default:
            break;
    }
}

static int key_for_scancode(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_X: return 0x0;
        case SDL_SCANCODE_1: return 0x1;
        case SDL_SCANCODE_2: return 0x2;
        case SDL_SCANCODE_3: return 0x3;
        case SDL_SCANCODE_Q: return 0x4;
        case SDL_SCANCODE_W: return 0x5;
        case SDL_SCANCODE_E: return 0x6;
        case SDL_SCANCODE_A: return 0x7;
        case SDL_SCANCODE_S: return 0x8;
        case SDL_SCANCODE_D: return 0x9;
        case SDL_SCANCODE_Z: return 0xA;
        case SDL_SCANCODE_C: return 0xB;
        case SDL_SCANCODE_4: return 0xC;
        case SDL_SCANCODE_R: return 0xD;
        case SDL_SCANCODE_F: return 0xE;
        case SDL_SCANCODE_V: return 0xF;
        default: return -1;
    }
}

static void release_keys(App *app) {
    for (uint8_t key = 0; key < 16; key++) {
        chip8_set_key(&app->machine, key, 0);
    }
    memset(app->kb_held, 0, sizeof(app->kb_held));
    app->ff_key = false;
    app->pad_held = 0;
    app->pad_seen = 0;
    if (app->gb) {
        for (int button = 0; button < GB_BUTTON_COUNT; button++) {
            gb_set_button(app->gb, (GbButton)button, 0);
        }
    }
}

static void handle_file_event(App *app, SDL_Event *event) {
    char *value = (char *)event->user.data1;
    if (event->user.code == FILE_EVENT_OPENED && value) {
        char error[256];
        if (!load_rom(app, value, error, sizeof(error))) show_error(app, error);
    } else if (event->user.code == FILE_EVENT_ERROR && value) {
        show_error(app, value);
    }
    SDL_free(value);
}

static void handle_key_event(App *app, const SDL_KeyboardEvent *key,
                             bool pressed) {
    SDL_Keymod modifiers = key->mod;
    bool command = (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    if (app->rebinding) {
        if (pressed && !key->repeat) handle_controls_key(app, key->scancode);
        return;
    }
    if (app->is_gb) {
        int action = gbi_lookup(&app->keys, (int)key->scancode);
        if (action >= 0) {
            if (!pressed) {
                gb_action_release(app, action);
                return;
            }
            /* Ctrl/Alt/Super chords stay shortcuts (Alt+Tab must not fast-forward). */
            if (!command && !(modifiers & SDL_KMOD_ALT)) {
                gb_action_press(app, action, key->repeat);
                return;
            }
        }
    } else {
        int emulated_key = key_for_scancode(key->scancode);
        if (emulated_key >= 0) {
            chip8_set_key(&app->machine, (uint8_t)emulated_key, pressed);
        }
    }
    if (!pressed || key->repeat) return;

    switch (key->scancode) {
        case SDL_SCANCODE_ESCAPE:
            app->quit_requested = true;
            break;
        case SDL_SCANCODE_SPACE:
            app->user_paused = !app->user_paused;
            if (app->user_paused) release_keys(app);
            sync_audio(app);
            update_title(app);
            break;
        case SDL_SCANCODE_F2: {
            char error[256];
            if (!restart(app, error, sizeof(error))) {
                show_error(app, error);
            }
            break;
        }
        case SDL_SCANCODE_F3:
            if (app->is_gb) {
                app->gb_speed = (app->gb_speed + 1) % 3;
                update_title(app);
            }
            break;
        case SDL_SCANCODE_F4:
            if (app->is_gb) {
                app->gb_palette = (app->gb_palette + 1) % 3;
                update_title(app);
            }
            break;
        case SDL_SCANCODE_F10:
            open_controls(app);
            break;
        case SDL_SCANCODE_M:
            app->muted = !app->muted;
            sync_audio(app);
            break;
        case SDL_SCANCODE_O:
            if (command) open_rom_dialog(app);
            break;
        case SDL_SCANCODE_Q:
            if (command) app->quit_requested = true;
            break;
        default:
            break;
    }
}

static void handle_event(App *app, SDL_Event *event) {
    if (event->type == app->rom_event_type) {
        handle_file_event(app, event);
        return;
    }

    switch (event->type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            app->quit_requested = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            app->focus_paused = true;
            release_keys(app);
            sync_audio(app);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            app->focus_paused = false;
            sync_audio(app);
            break;
        case SDL_EVENT_KEY_DOWN:
            handle_key_event(app, &event->key, true);
            break;
        case SDL_EVENT_KEY_UP:
            handle_key_event(app, &event->key, false);
            break;
        case SDL_EVENT_DROP_FILE:
            if (event->drop.data) {
                char error[256];
                if (!load_rom(app, event->drop.data, error, sizeof(error))) show_error(app, error);
            }
            break;
        default:
            break;
    }
}

static uint32_t gb_shade(int palette, int shade) {
    uint32_t light = gb_palette_light[palette], out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        int a = (int)((light >> shift) & 0xFF), b = (int)((GB_PALETTE_DARK >> shift) & 0xFF);
        out |= (uint32_t)(a + (b - a) * shade / 3) << shift;
    }
    return out;
}

static int upload_gb_frame(App *app) {
    static uint8_t pixels[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT * 4];
    const uint8_t *shades = gb_framebuffer(app->gb);
    uint32_t colors[4];
    for (int i = 0; i < 4; i++) colors[i] = gb_shade(app->gb_palette, i);
    for (size_t i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
        uint32_t color = colors[shades[i] & 3];
        pixels[i * 4] = (uint8_t)(color >> 16);
        pixels[i * 4 + 1] = (uint8_t)(color >> 8);
        pixels[i * 4 + 2] = (uint8_t)color;
        pixels[i * 4 + 3] = 255;
    }
    return SDL_UpdateTexture(app->gb_framebuffer, NULL, pixels, GB_SCREEN_WIDTH * 4);
}

static int upload_chip8_frame(App *app) {
    uint8_t pixels[CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT * 4];
    for (size_t i = 0; i < CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT; i++) {
        uint8_t value = app->machine.display[i] ? 255 : 0;
        pixels[i * 4] = value;
        pixels[i * 4 + 1] = value;
        pixels[i * 4 + 2] = value;
        pixels[i * 4 + 3] = 255;
    }
    return SDL_UpdateTexture(app->framebuffer, NULL, pixels, CHIP8_DISPLAY_WIDTH * 4);
}

/* Debug text is an 8x8 bitmap font; scale it to the window so it stays readable. */
static float text_scale(int width) {
    float scale = SDL_floorf((float)width / 440.0f);
    return scale < 1.0f ? 1.0f : (scale > 4.0f ? 4.0f : scale);
}

static void draw_text(App *app, float x, float y, const char *text, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(app->renderer, r, g, b, 255);
    SDL_RenderDebugText(app->renderer, x, y, text);
}

/* The status line and the FAST badge sit over the game. */
static void draw_hud(App *app, int width, int height) {
    bool status = app->status[0] && SDL_GetTicksNS() < app->status_until;
    if (!status && app->status[0]) app->status[0] = '\0';
    if (!status && !app->ff_on) return;

    float scale = text_scale(width);
    SDL_SetRenderScale(app->renderer, scale, scale);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    float lw = (float)width / scale, lh = (float)height / scale;
    if (status) {
        float w = (float)strlen(app->status) * 8.0f + 8.0f;
        SDL_FRect box = { 4.0f, lh - 16.0f, w, 12.0f };
        SDL_SetRenderDrawColor(app->renderer, 0x10, 0x16, 0x14, 200);
        SDL_RenderFillRect(app->renderer, &box);
        draw_text(app, 8.0f, lh - 14.0f, app->status, 0xEB, 0xD8, 0xAF);
    }
    if (app->ff_on) {
        SDL_FRect box = { lw - 72.0f, 4.0f, 68.0f, 12.0f };
        SDL_SetRenderDrawColor(app->renderer, 0x10, 0x16, 0x14, 200);
        SDL_RenderFillRect(app->renderer, &box);
        draw_text(app, lw - 68.0f, 6.0f, "FAST  4x", 0xEB, 0xD8, 0xAF);
    }
    SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
}

/* F10: every action with its two keys. Rows are picked with the arrows. */
static void draw_controls(App *app, int width, int height) {
    float scale = text_scale(width);
    SDL_FRect all = { 0.0f, 0.0f, (float)width, (float)height };
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, 0x0C, 0x10, 0x0E, 235);
    SDL_RenderFillRect(app->renderer, &all);
    SDL_SetRenderScale(app->renderer, scale, scale);

    float x = 12.0f, y = 8.0f;
    draw_text(app, x, y, "GAME BOY CONTROLS", 0xEB, 0xD8, 0xAF);
    y += 14.0f;
    draw_text(app, x, y, "Up/Down row  Left/Right key  Enter rebind", 0x9A, 0xB0, 0xA2);
    y += 10.0f;
    draw_text(app, x, y, "Backspace clear  R defaults  F10/Esc close", 0x9A, 0xB0, 0xA2);
    y += 16.0f;
    for (int action = 0; action < GBI_COUNT; action++) {
        char cells[GBI_KEYS_PER_ACTION][40], line[160];
        for (int s = 0; s < GBI_KEYS_PER_ACTION; s++) {
            char name[48];
            key_name(app->keys.key[action][s], name, sizeof(name));
            bool here = action == app->rb_action && s == app->rb_slot;
            if (here && app->capturing) snprintf(cells[s], sizeof(cells[s]), "[press a key]");
            else if (here) snprintf(cells[s], sizeof(cells[s]), "[%.14s]", name);
            else snprintf(cells[s], sizeof(cells[s]), " %.14s ", name);
        }
        snprintf(line, sizeof(line), "%c %-13s %-16s %-16s", action == app->rb_action ? '>' : ' ',
                 gbi_name((GbInput)action), cells[0], cells[1]);
        if (action == app->rb_action) draw_text(app, x, y, line, 0xEB, 0xD8, 0xAF);
        else draw_text(app, x, y, line, 0xC3, 0xDC, 0xC7);
        y += 11.0f;
    }
    y += 5.0f;
    if (app->rb_note[0]) draw_text(app, x, y, app->rb_note, 0xE0, 0x90, 0x70);
    y += 12.0f;
    draw_text(app, x, y, "Pad: D-pad/stick, B/A buttons, R shoulder = fast", 0x9A, 0xB0, 0xA2);
    y += 10.0f;
    draw_text(app, x, y, "L shoulder + Start/Back/Y = save/load/slot", 0x9A, 0xB0, 0xA2);
    SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
}

static int render_frame(App *app) {
    SDL_Texture *texture = app->is_gb ? app->gb_framebuffer : app->framebuffer;
    float source_w = app->is_gb ? (float)GB_SCREEN_WIDTH : (float)CHIP8_DISPLAY_WIDTH;
    float source_h = app->is_gb ? (float)GB_SCREEN_HEIGHT : (float)CHIP8_DISPLAY_HEIGHT;
    if (!(app->is_gb ? upload_gb_frame(app) : upload_chip8_frame(app))) return 0;

    int width, height;
    if (!SDL_GetRenderOutputSize(app->renderer, &width, &height)) return 0;
    float scale_x = (float)width / source_w;
    float scale_y = (float)height / source_h;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    /* Whole-number scaling keeps Game Boy pixels even when there is room for it. */
    if (app->is_gb && scale >= 1.0f) scale = SDL_floorf(scale);
    SDL_FRect destination = {
        ((float)width - source_w * scale) / 2.0f,
        ((float)height - source_h * scale) / 2.0f,
        source_w * scale,
        source_h * scale
    };

    if (app->is_gb) SDL_SetRenderDrawColor(app->renderer, 0x18, 0x21, 0x1E, 255);
    else SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    if (!SDL_RenderClear(app->renderer) ||
        !SDL_RenderTexture(app->renderer, texture, NULL, &destination)) {
        return 0;
    }
    if (app->rebinding) draw_controls(app, width, height);
    else draw_hud(app, width, height);
    SDL_RenderPresent(app->renderer);
    return 1;
}

static char *default_rom_path(void) {
    const char *base = SDL_GetBasePath();
    if (!base) return NULL;
    char *path = NULL;
#ifdef __APPLE__
    SDL_asprintf(&path, "%s../Resources/hello.ch8", base);
#else
    SDL_asprintf(&path, "%sassets/hello.ch8", base);
#endif
    return path;
}

static SDL_Texture *make_texture(App *app, int width, int height) {
    SDL_Texture *texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STREAMING, width, height);
    if (texture && !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
        SDL_DestroyTexture(texture);
        return NULL;
    }
    return texture;
}

static int initialize(App *app, const char *rom_path) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "could not initialize SDL: %s\n", SDL_GetError());
        return 0;
    }
    app->sdl_initialized = true;
    pad_init();
    {
        char *dir = SDL_GetPrefPath("tdotlab", "Nacre");
        if (dir) {
            SDL_asprintf(&app->prefs_path, "%scontrols.txt", dir);
            SDL_free(dir);
        }
        load_prefs(app);
    }

    app->window = SDL_CreateWindow("Nacre", WINDOW_WIDTH, WINDOW_HEIGHT,
                                    SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app->window) {
        fprintf(stderr, "could not create the Nacre window: %s\n", SDL_GetError());
        return 0;
    }
    app->renderer = SDL_CreateRenderer(app->window, NULL);
    if (!app->renderer) {
        fprintf(stderr, "could not create the Nacre renderer: %s\n", SDL_GetError());
        return 0;
    }
    app->framebuffer = make_texture(app, CHIP8_DISPLAY_WIDTH, CHIP8_DISPLAY_HEIGHT);
    app->gb_framebuffer = make_texture(app, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
    if (!app->framebuffer || !app->gb_framebuffer) {
        fprintf(stderr, "could not create the framebuffer textures: %s\n", SDL_GetError());
        return 0;
    }

    app->rom_event_type = SDL_RegisterEvents(1);
    if (app->rom_event_type == (Uint32)-1) {
        fprintf(stderr, "could not register the ROM event: %s\n", SDL_GetError());
        return 0;
    }

    app->seed = 0x4E414352u;
    app->gb_speed = 1;
    app->gb = gb_create();
    if (!app->gb) fprintf(stderr, "the Game Boy core could not start\n");

    /* The Game Boy pushes stereo samples itself; its stream stays paused until a
     * cartridge runs. Open it before loading so a cartridge starts with sound. */
    SDL_AudioSpec gb_spec;
    SDL_zero(gb_spec);
    gb_spec.format = SDL_AUDIO_S16;
    gb_spec.channels = 2;
    gb_spec.freq = (int)GB_AUDIO_SAMPLE_RATE;
    app->gb_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                               &gb_spec, NULL, NULL);

    if (rom_path) {
        char error[256];
        if (!load_rom(app, rom_path, error, sizeof(error))) {
            show_error(app, error);
            return 0;
        }
    } else {
        char *bundled_rom = default_rom_path();
        char error[256];
        if (!bundled_rom) {
            snprintf(error, sizeof(error), "could not locate the bundled hello.ch8 ROM");
            show_error(app, error);
            return 0;
        }
        if (!load_rom(app, bundled_rom, error, sizeof(error))) {
            SDL_free(bundled_rom);
            show_error(app, error);
            return 0;
        }
        SDL_free(bundled_rom);
    }

    chip8_audio_oscillator_reset(&app->audio.oscillator);
    SDL_AudioSpec audio_spec;
    SDL_zero(audio_spec);
    audio_spec.format = SDL_AUDIO_S16;
    audio_spec.channels = 1;
    audio_spec.freq = CHIP8_AUDIO_SAMPLE_RATE;
    app->audio.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec,
        audio_callback, &app->audio);
    if (app->audio.stream && SDL_ResumeAudioStreamDevice(app->audio.stream)) {
        app->audio_available = true;
    } else {
        fprintf(stderr, "audio is unavailable; continuing silently: %s\n",
                SDL_GetError());
    }
    sync_audio(app);

    return 1;
}

static void cleanup(App *app) {
    write_save(app);
    SDL_SetAtomicInt(&app->audio.active, 0);
    if (app->audio.stream) SDL_DestroyAudioStream(app->audio.stream);
    if (app->gb_stream) SDL_DestroyAudioStream(app->gb_stream);
    if (app->framebuffer) SDL_DestroyTexture(app->framebuffer);
    if (app->gb_framebuffer) SDL_DestroyTexture(app->gb_framebuffer);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    gb_destroy(app->gb);
    SDL_free(app->rom_path);
    SDL_free(app->save_path);
    SDL_free(app->prefs_path);
    pad_shutdown();
    if (app->sdl_initialized) SDL_Quit();
}

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "Usage: Nacre [path/to/rom.ch8 | path/to/cartridge.gb]\n");
        return 2;
    }

    App app;
    memset(&app, 0, sizeof(app));
    if (!initialize(&app, argc == 2 ? argv[1] : NULL)) {
        cleanup(&app);
        return 1;
    }

    uint64_t previous = SDL_GetTicksNS();
    while (1) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) handle_event(&app, &event);
        if (app.quit_requested && !SDL_GetAtomicInt(&app.dialog_pending)) break;

        uint64_t now = SDL_GetTicksNS();
        uint64_t elapsed = now - previous;
        previous = now;
        char error[256];
        if (!app.is_gb) app.ff_on = false;
        if (app.is_gb) {
            poll_pad(&app);
            app.ff_on = (app.ff_key || (app.pad_held & PAD_BIT(GBI_FAST_FORWARD))) && !is_paused(&app);
            unsigned percent = app.ff_on ? GB_FAST_PERCENT : gb_speed_percent[app.gb_speed];
            if (!is_paused(&app) &&
                !gb_advance_ns(app.gb, elapsed, percent, error, sizeof(error))) {
                show_error(&app, error);
                app.quit_requested = true;
            }
            pump_gb_audio(&app);
            /* Games write save RAM in bursts; settle for a second before writing. */
            if (app.save_path && gb_save_dirty(app.gb)) {
                if (!app.save_due) app.save_due = now + 1000000000u;
                else if (now >= app.save_due) write_save(&app);
            }
        } else if (!chip8_scheduler_advance(&app.machine, &app.scheduler, elapsed,
                                            is_paused(&app), timer_tick, &app,
                                            error, sizeof(error))) {
            show_error(&app, error);
            app.quit_requested = true;
        }
        sync_audio(&app);
        if (!render_frame(&app)) {
            fprintf(stderr, "could not render the Nacre framebuffer: %s\n",
                    SDL_GetError());
            app.quit_requested = true;
        }
        SDL_Delay(8);
    }

    cleanup(&app);
    return 0;
}
