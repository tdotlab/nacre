/* The SDL3 frontend (Linux). It knows no emulated system: games run through a
 * NacreSession, so every core in the registry works here, and the window only
 * draws frames, plays sound, and turns keys and controllers into input. */

#include "audio_out.h"
#include "gb_input.h"
#include "nacre_core.h"
#include "nacre_session.h"
#include "pad.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WINDOW_WIDTH 960
#define WINDOW_HEIGHT 480
#define MAX_CONTENT (256u << 20) /* games read whole; disc images go by path */
#define FAST_PERCENT 400u        /* while fast-forward is held */
#define MAX_FILTERS 8

enum {
    FILE_EVENT_OPENED = 1,
    FILE_EVENT_ERROR = 2
};

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    unsigned tex_w, tex_h;
    uint32_t *pixels; /* XRGB8888 staging for indexed frames */
    size_t pixels_cap;
    uint32_t shown_frame; /* frame_count last uploaded */
    int shown_palette;
    const void *shown_owner;
    bool sdl_initialized;
    Uint32 rom_event_type;
    SDL_AtomicInt dialog_pending;

    NacreSession *session;
    AudioOut *audio;
    bool muted;
    bool user_paused;
    bool focus_paused;
    bool quit_requested;
    int speed;   /* index into speed_percent */
    int palette; /* index into palette_light */
    int applied_running;
    unsigned applied_speed;
    char last_save_note[320];

    /* Input: rebindable keys, a controller, hold-to-fast-forward. Held actions are
     * GBI_BIT flags; the button ones become pad input for cores that have a pad. */
    GbBindings keys;
    unsigned kb_held;
    unsigned pad_held, pad_seen; /* controller actions now / last poll */
    bool ff_key, ff_on;
    int slot; /* save-state slot, 0..NACRE_STATE_SLOTS-1 */
    char status[96];
    uint64_t status_until;
    char *prefs_path; /* controls.txt in the user's preference folder */

    /* The controls overlay (F10). */
    bool rebinding;
    bool capturing; /* waiting for the key to bind */
    int rb_action, rb_slot;
    char rb_note[96];
} App;

/* File-dialog filters, built from the core registry at start. */
static SDL_DialogFileFilter rom_filters[MAX_FILTERS];
static int rom_filter_count;
static char all_extensions[256];

static const unsigned speed_percent[3] = { 50, 100, 200 };
static const char *const speed_name[3] = { "0.5x", "1x", "2x" };
/* The desktop apps' Fern, Amber, and Pearl inks, shaded down to a dark green. */
static const uint32_t palette_light[3] = { 0xC3DCC7, 0xEBD8AF, 0xE8E9DF };
static const char *const palette_name[3] = { "Fern", "Amber", "Pearl" };
#define PALETTE_DARK 0x1E2A25u
#define SCREEN_BACKGROUND 0x18211Eu

/* Arrows and WASD for the D-pad, X/K for A, Z/J for B, Enter for Start, Backspace or
 * right Shift for Select; Tab fast-forwards, F5 and F7 save and load, F6 changes slot.
 * On a CHIP-8 keypad (1234/QWER/ASDF/ZXCV) the keypad takes those keys first. */
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

static const NacreCore *current_core(const App *app) {
    return app->session ? nacre_session_core(app->session) : NULL;
}

static void *current_instance(const App *app) {
    return app->session ? nacre_session_instance(app->session) : NULL;
}

static int is_paused(const App *app) {
    return app->user_paused || app->focus_paused || app->rebinding;
}

/* ---- Files: host I/O for the session --------------------------------------- */

static NacreIoResult io_read(void *userdata, const char *path, size_t max_size, uint8_t **data,
                             size_t *size) {
    (void)userdata;
    *data = NULL;
    *size = 0;
    if (!SDL_GetPathInfo(path, NULL)) return NACRE_IO_MISSING;
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (!file) return NACRE_IO_FAILED;
    Sint64 length = SDL_GetIOSize(file);
    if (length < 0 || (uint64_t)length > max_size) {
        SDL_CloseIO(file);
        return NACRE_IO_FAILED;
    }
    uint8_t *bytes = (uint8_t *)malloc(length ? (size_t)length : 1);
    bool ok = bytes && SDL_ReadIO(file, bytes, (size_t)length) == (size_t)length;
    SDL_CloseIO(file);
    if (!ok) {
        free(bytes);
        return NACRE_IO_FAILED;
    }
    *data = bytes;
    *size = (size_t)length;
    return NACRE_IO_OK;
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

static int io_write(void *userdata, const char *path, const uint8_t *data, size_t size) {
    (void)userdata;
    return write_atomic(path, data, size) ? 1 : 0;
}

static int64_t io_now(void *userdata) {
    (void)userdata;
    return (int64_t)time(NULL);
}

static void host_log(void *userdata, NacreLogLevel level, const char *message) {
    (void)userdata;
    if (level >= NACRE_LOG_INFO) fprintf(stderr, "%s\n", message);
}

/* ---- Status and title ------------------------------------------------------ */

/* A short message drawn over the game that clears itself after a few seconds. */
static void set_status(App *app, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(app->status, sizeof(app->status), format, args);
    va_end(args);
    app->status_until = SDL_GetTicksNS() + 3000000000ull;
}

static const char *file_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

static void update_title(App *app) {
    char title[256];
    const NacreCore *core = current_core(app);
    if (!core) {
        snprintf(title, sizeof(title), "Nacre");
    } else {
        const char *label = core->title ? core->title(current_instance(app)) : NULL;
        const char *path = nacre_session_path(app->session);
        if (!label && path) label = file_name(path);
        if (label) {
            snprintf(title, sizeof(title), "Nacre - %s (%s, %s, %s)%s", label, core->name,
                     speed_name[app->speed], palette_name[app->palette],
                     app->user_paused ? " - paused" : "");
        } else {
            snprintf(title, sizeof(title), "Nacre - %s (%s, %s)%s", core->name,
                     speed_name[app->speed], palette_name[app->palette],
                     app->user_paused ? " - paused" : "");
        }
    }
    SDL_SetWindowTitle(app->window, title);
}

static void show_error(App *app, const char *message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Nacre", message, app->window);
}

/* Puts a battery-save note ("Loaded Tetris.sav", "Couldn't write Tetris.sav") on
 * screen whenever it changes. */
static void show_save_note(App *app) {
    const char *note = nacre_session_save_note(app->session);
    if (!strcmp(note, app->last_save_note)) return;
    snprintf(app->last_save_note, sizeof(app->last_save_note), "%s", note);
    if (note[0]) {
        set_status(app, "%s", note);
        fprintf(stderr, "%s\n", note);
    }
}

/* ---- Opening games --------------------------------------------------------- */

/* Picks the core from the file's first bytes, then hands the session the whole
 * file, or only its path for cores that read content themselves (disc images).
 * A failed open keeps the game that was running. */
static int open_content(App *app, const char *path, char *error, size_t error_capacity) {
    uint8_t head[NACRE_PROBE_BYTES];
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (!file) {
        snprintf(error, error_capacity, "cannot open '%s': %s", path, SDL_GetError());
        return 0;
    }
    Sint64 length = SDL_GetIOSize(file);
    size_t want = length > 0 ? ((uint64_t)length < sizeof(head) ? (size_t)length : sizeof(head)) : 0;
    size_t got = want ? SDL_ReadIO(file, head, want) : 0;
    SDL_CloseIO(file);
    if (length <= 0 || got != want) {
        snprintf(error, error_capacity, "'%s' is empty or cannot be read", path);
        return 0;
    }

    const NacreCore *core = nacre_core_detect(head, got, (size_t)length, path);
    if (!core) {
        snprintf(error, error_capacity,
                 "Nacre doesn't recognize this file as a game for any system it runs.");
        return 0;
    }
    int ok;
    if (core->flags & NACRE_CORE_NEEDS_PATH) {
        ok = nacre_session_open(app->session, core, path, NULL, 0, error, error_capacity);
    } else {
        uint8_t *data = NULL;
        size_t size = 0;
        if (io_read(app, path, MAX_CONTENT, &data, &size) != NACRE_IO_OK) {
            snprintf(error, error_capacity, "'%s' is too large or cannot be read", path);
            return 0;
        }
        ok = nacre_session_open(app->session, core, path, data, size, error, error_capacity);
        free(data);
    }
    if (!ok) return 0;
    app->user_paused = false;
    app->last_save_note[0] = '\0';
    show_save_note(app);
    update_title(app);
    return 1;
}

/* The welcome program ships beside the binary. It opens as bytes with no path,
 * so nothing (saves, states) is ever written next to the install. */
static int open_bundled(App *app, char *error, size_t error_capacity) {
    const char *base = SDL_GetBasePath();
    char *path = NULL;
    uint8_t *data = NULL;
    size_t size = 0;
    int ok = 0;
    if (!base) {
        snprintf(error, error_capacity, "could not locate the bundled hello.ch8 ROM");
        return 0;
    }
#ifdef __APPLE__
    SDL_asprintf(&path, "%s../Resources/hello.ch8", base);
#else
    SDL_asprintf(&path, "%sassets/hello.ch8", base);
#endif
    if (!path || io_read(app, path, 4096, &data, &size) != NACRE_IO_OK) {
        snprintf(error, error_capacity, "could not read the bundled ROM '%s'", path ? path : "hello.ch8");
    } else {
        ok = nacre_session_open(app->session, nacre_core_by_id("chip8"), NULL, data, size, error,
                                error_capacity);
    }
    free(data);
    SDL_free(path);
    if (ok) update_title(app);
    return ok;
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

static void SDLCALL rom_dialog_callback(void *userdata, const char *const *filelist, int filter) {
    (void)filter;
    App *app = (App *)userdata;
    if (!filelist) {
        queue_file_event(app, FILE_EVENT_ERROR, SDL_GetError());
    } else if (filelist[0]) {
        queue_file_event(app, FILE_EVENT_OPENED, filelist[0]);
    }
    SDL_SetAtomicInt(&app->dialog_pending, 0);
}

/* "Games" lists every core's extensions (plus the loose ones CHIP-8 programs and
 * colour-compatible cartridges come in), then one filter per system. */
static void build_filters(void) {
    const NacreCore *const *cores = nacre_cores();
    size_t used = 0;
    rom_filter_count = 1;
    for (size_t i = 0; cores[i] && rom_filter_count < MAX_FILTERS - 1; i++) {
        if (!cores[i]->extensions || !cores[i]->extensions[0]) continue;
        used += (size_t)snprintf(all_extensions + used, sizeof(all_extensions) - used, "%s%s",
                                 used ? ";" : "", cores[i]->extensions);
        if (used >= sizeof(all_extensions)) used = sizeof(all_extensions) - 1;
        rom_filters[rom_filter_count].name = cores[i]->name;
        rom_filters[rom_filter_count].pattern = cores[i]->extensions;
        rom_filter_count++;
    }
    snprintf(all_extensions + used, sizeof(all_extensions) - used, "%sgbc;rom;bin", used ? ";" : "");
    rom_filters[0].name = "Games";
    rom_filters[0].pattern = all_extensions;
    rom_filters[rom_filter_count].name = "All files";
    rom_filters[rom_filter_count].pattern = "*";
    rom_filter_count++;
}

static void open_rom_dialog(App *app) {
    if (SDL_GetAtomicInt(&app->dialog_pending)) return;
    SDL_SetAtomicInt(&app->dialog_pending, 1);
    SDL_ShowOpenFileDialog(rom_dialog_callback, app, app->window, rom_filters, rom_filter_count,
                           NULL, false);
}

/* ---- Input: states, fast-forward, rebinding -------------------------------- */

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
    uint8_t *data = NULL;
    size_t length = 0;
    gbi_set_defaults(&app->keys, default_keys);
    app->slot = 0;
    if (!app->prefs_path || io_read(app, app->prefs_path, 4096, &data, &length) != NACRE_IO_OK)
        return;
    char *text = (char *)malloc(length + 1);
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
            if (slot && slot[5] >= '0' && slot[5] < '0' + NACRE_STATE_SLOTS) app->slot = slot[5] - '0';
        }
        free(text);
    }
    free(data);
}

/* Hands the held buttons to the core (keyboard and controller merged). */
static void apply_buttons(App *app) {
    const NacreCore *core = current_core(app);
    if (!core || !core->set_pad) return;
    core->set_pad(current_instance(app), 0, gbi_pad_buttons(app->kb_held | app->pad_held));
}

static void save_state(App *app) {
    char error[128];
    if (nacre_session_save_state(app->session, app->slot, error, sizeof(error)))
        set_status(app, "Saved state to slot %d", app->slot + 1);
    else
        set_status(app, "%s", error);
}

static void load_state(App *app) {
    char error[128];
    if (nacre_session_load_state(app->session, app->slot, error, sizeof(error)))
        set_status(app, "Loaded state from slot %d", app->slot + 1);
    else
        set_status(app, "%s", error);
}

static void next_slot(App *app) {
    app->slot = (app->slot + 1) % NACRE_STATE_SLOTS;
    save_prefs(app);
    set_status(app, "State slot %d", app->slot + 1);
}

static void action_press(App *app, int action, bool repeat) {
    if (action < GBI_BUTTONS) {
        if (!is_paused(app) && !repeat) {
            app->kb_held |= GBI_BIT(action);
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

static void action_release(App *app, int action) {
    if (action < GBI_BUTTONS) {
        app->kb_held &= ~GBI_BIT(action);
        apply_buttons(app);
    } else if (action == GBI_FAST_FORWARD) {
        app->ff_key = false;
    }
}

/* Reads the controllers: held buttons feed the core, and the state hotkeys fire once
 * per press. Only the focused, running window listens, so a game in the background isn't played. */
static void poll_pad(App *app) {
    unsigned held = pad_poll();
    const char *note = pad_take_message();
    if (note) set_status(app, "%s", note);
    if (!current_core(app) || is_paused(app) || SDL_GetAtomicInt(&app->dialog_pending)) held = 0;
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

static void release_keys(App *app) {
    const NacreCore *core = current_core(app);
    if (core && core->set_keypad) {
        for (unsigned key = 0; key < core->keypad_keys; key++)
            core->set_keypad(current_instance(app), key, 0);
    }
    app->kb_held = 0;
    app->ff_key = false;
    app->pad_held = 0;
    app->pad_seen = 0;
    apply_buttons(app);
}

static void open_controls(App *app) {
    release_keys(app);
    app->rebinding = true;
    app->capturing = false;
    app->rb_note[0] = '\0';
}

static void close_controls(App *app) {
    app->rebinding = false;
    app->capturing = false;
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

/* The 16-key hex keypad on the left of a QWERTY keyboard:
 *   1 2 3 C      1 2 3 4
 *   4 5 6 D  <-  Q W E R
 *   7 8 9 E      A S D F
 *   A 0 B F      Z X C V */
static int keypad_key(SDL_Scancode scancode) {
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

static void handle_file_event(App *app, SDL_Event *event) {
    char *value = (char *)event->user.data1;
    if (event->user.code == FILE_EVENT_OPENED && value) {
        char error[256];
        if (!open_content(app, value, error, sizeof(error))) show_error(app, error);
    } else if (event->user.code == FILE_EVENT_ERROR && value) {
        show_error(app, value);
    }
    SDL_free(value);
}

static void handle_key_event(App *app, const SDL_KeyboardEvent *key, bool pressed) {
    SDL_Keymod modifiers = key->mod;
    bool command = (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    const NacreCore *core = current_core(app);
    if (app->rebinding) {
        if (pressed && !key->repeat) handle_controls_key(app, key->scancode);
        return;
    }
    int hex = core && core->set_keypad ? keypad_key(key->scancode) : -1;
    if (hex >= 0 && (unsigned)hex < core->keypad_keys) {
        if (!key->repeat) core->set_keypad(current_instance(app), (unsigned)hex, pressed);
    } else if (core) {
        int action = gbi_lookup(&app->keys, (int)key->scancode);
        if (action >= 0) {
            if (!pressed) {
                action_release(app, action);
                return;
            }
            /* Ctrl/Alt/Super chords stay shortcuts (Alt+Tab must not fast-forward). */
            if (!command && !(modifiers & SDL_KMOD_ALT)) {
                action_press(app, action, key->repeat);
                return;
            }
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
            update_title(app);
            break;
        case SDL_SCANCODE_F2:
            nacre_session_restart(app->session);
            app->user_paused = false;
            update_title(app);
            break;
        case SDL_SCANCODE_F3:
            app->speed = (app->speed + 1) % 3;
            update_title(app);
            break;
        case SDL_SCANCODE_F4:
            app->palette = (app->palette + 1) % 3;
            update_title(app);
            break;
        case SDL_SCANCODE_F10:
            open_controls(app);
            break;
        case SDL_SCANCODE_M:
            app->muted = !app->muted;
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
            nacre_session_flush(app->session);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            app->focus_paused = false;
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
                if (!open_content(app, event->drop.data, error, sizeof(error))) show_error(app, error);
            }
            break;
        default:
            break;
    }
}

/* ---- Drawing --------------------------------------------------------------- */

static uint32_t mix(uint32_t a, uint32_t b, unsigned step, unsigned steps) {
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        int x = (int)((a >> shift) & 0xFF), y = (int)((b >> shift) & 0xFF);
        out |= (uint32_t)(x + (y - x) * (int)step / (int)steps) << shift;
    }
    return out;
}

/* Shade i of an indexed frame. LCD shades run from the palette's light ink (0) to
 * the dark green; a two-level display lights its set pixels on the dark screen, as
 * the Mac and Windows interfaces draw CHIP-8. */
static uint32_t shade_color(int palette, unsigned shade, unsigned levels) {
    uint32_t light = palette_light[palette];
    if (levels <= 2) return shade ? light : PALETTE_DARK;
    if (shade >= levels) shade = levels - 1;
    return mix(light, PALETTE_DARK, shade, levels - 1);
}

static bool ensure_texture(App *app, unsigned width, unsigned height) {
    if (app->texture && app->tex_w == width && app->tex_h == height) return true;
    if (app->texture) SDL_DestroyTexture(app->texture);
    app->texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_XRGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
    app->tex_w = app->tex_h = 0;
    if (!app->texture) return false;
    if (!SDL_SetTextureScaleMode(app->texture, SDL_SCALEMODE_NEAREST)) {
        SDL_DestroyTexture(app->texture);
        app->texture = NULL;
        return false;
    }
    app->tex_w = width;
    app->tex_h = height;
    app->shown_owner = NULL; /* force an upload */
    return true;
}

/* Uploads the frame when it, the palette, or the game changed. */
static bool upload_frame(App *app, const NacreVideoFrame *frame) {
    if (!ensure_texture(app, frame->width, frame->height)) return false;
    const void *owner = current_instance(app);
    if (owner == app->shown_owner && frame->frame_count == app->shown_frame &&
        app->palette == app->shown_palette)
        return true;
    bool ok;
    if (frame->format == NACRE_PIXEL_XRGB8888) {
        ok = SDL_UpdateTexture(app->texture, NULL, frame->pixels, (int)frame->pitch);
    } else {
        size_t count = (size_t)frame->width * frame->height;
        if (count > app->pixels_cap) {
            uint32_t *grown = (uint32_t *)realloc(app->pixels, count * sizeof(uint32_t));
            if (!grown) return false;
            app->pixels = grown;
            app->pixels_cap = count;
        }
        uint32_t colors[256];
        unsigned levels = frame->levels ? frame->levels : 2;
        if (levels > 256) levels = 256;
        for (unsigned i = 0; i < 256; i++) colors[i] = shade_color(app->palette, i < levels ? i : levels - 1, levels);
        const uint8_t *rows = (const uint8_t *)frame->pixels;
        for (unsigned y = 0; y < frame->height; y++) {
            const uint8_t *row = rows + y * frame->pitch;
            uint32_t *out = app->pixels + (size_t)y * frame->width;
            for (unsigned x = 0; x < frame->width; x++) out[x] = colors[row[x]];
        }
        ok = SDL_UpdateTexture(app->texture, NULL, app->pixels, (int)(frame->width * 4));
    }
    if (ok) {
        app->shown_owner = owner;
        app->shown_frame = frame->frame_count;
        app->shown_palette = app->palette;
    }
    return ok;
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
    draw_text(app, x, y, "CONTROLS", 0xEB, 0xD8, 0xAF);
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
    y += 10.0f;
    draw_text(app, x, y, "CHIP-8 keypad: 1234 QWER ASDF ZXCV", 0x9A, 0xB0, 0xA2);
    SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
}

static int render_frame(App *app) {
    const NacreCore *core = current_core(app);
    const NacreVideoFrame *frame = core && core->video ? core->video(current_instance(app)) : NULL;
    int width, height;
    if (!SDL_GetRenderOutputSize(app->renderer, &width, &height)) return 0;

    SDL_SetRenderDrawColor(app->renderer, (SCREEN_BACKGROUND >> 16) & 0xFF,
                           (SCREEN_BACKGROUND >> 8) & 0xFF, SCREEN_BACKGROUND & 0xFF, 255);
    if (!SDL_RenderClear(app->renderer)) return 0;
    if (frame && frame->width && frame->height && frame->pixels) {
        if (!upload_frame(app, frame)) return 0;
        float source_w = frame->aspect > 0.0f ? (float)frame->height * frame->aspect : (float)frame->width;
        float source_h = (float)frame->height;
        float scale_x = (float)width / source_w;
        float scale_y = (float)height / source_h;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        /* Whole-number scaling keeps square pixels even when there is room for it. */
        if (frame->aspect <= 0.0f && scale >= 1.0f) scale = SDL_floorf(scale);
        SDL_FRect destination = {
            ((float)width - source_w * scale) / 2.0f,
            ((float)height - source_h * scale) / 2.0f,
            source_w * scale,
            source_h * scale
        };
        if (!SDL_RenderTexture(app->renderer, app->texture, NULL, &destination)) return 0;
    }
    if (app->rebinding) draw_controls(app, width, height);
    else draw_hud(app, width, height);
    SDL_RenderPresent(app->renderer);
    return 1;
}

/* ---- Start, run, stop ------------------------------------------------------ */

static int initialize(App *app, const char *rom_path) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "could not initialize SDL: %s\n", SDL_GetError());
        return 0;
    }
    app->sdl_initialized = true;
    pad_init();
    build_filters();
    {
        char *dir = SDL_GetPrefPath("tdotlab", "Nacre");
        if (dir) {
            SDL_asprintf(&app->prefs_path, "%scontrols.txt", dir);
            SDL_free(dir);
        }
        load_prefs(app);
    }

    app->window = SDL_CreateWindow("Nacre", WINDOW_WIDTH, WINDOW_HEIGHT,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app->window) {
        fprintf(stderr, "could not create the Nacre window: %s\n", SDL_GetError());
        return 0;
    }
    app->renderer = SDL_CreateRenderer(app->window, NULL);
    if (!app->renderer) {
        fprintf(stderr, "could not create the Nacre renderer: %s\n", SDL_GetError());
        return 0;
    }
    app->rom_event_type = SDL_RegisterEvents(1);
    if (app->rom_event_type == (Uint32)-1) {
        fprintf(stderr, "could not register the ROM event: %s\n", SDL_GetError());
        return 0;
    }

    NacreSessionIo io = { io_read, io_write, io_now, app };
    NacreHost host = { NULL, NULL, host_log, NULL, app };
    app->session = nacre_session_create(&io, &host, 1);
    app->audio = audio_out_create();
    if (!app->session || !app->audio) {
        fprintf(stderr, "not enough memory to start\n");
        return 0;
    }
    app->speed = 1;
    app->applied_running = -1;

    char error[256];
    if (!(rom_path ? open_content(app, rom_path, error, sizeof(error))
                   : open_bundled(app, error, sizeof(error)))) {
        show_error(app, error);
        return 0;
    }
    return 1;
}

static void cleanup(App *app) {
    nacre_session_destroy(app->session); /* writes a pending battery save */
    audio_out_destroy(app->audio);
    if (app->texture) SDL_DestroyTexture(app->texture);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    free(app->pixels);
    SDL_free(app->prefs_path);
    pad_shutdown();
    if (app->sdl_initialized) SDL_Quit();
}

/* Pause and speed reach the session only when they change. */
static void sync_session(App *app) {
    int running = !is_paused(app);
    unsigned percent = app->ff_on ? FAST_PERCENT : speed_percent[app->speed];
    if (running != app->applied_running) {
        nacre_session_set_running(app->session, running);
        app->applied_running = running;
    }
    if (percent != app->applied_speed) {
        nacre_session_set_speed(app->session, percent);
        app->applied_speed = percent;
    }
}

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "Usage: Nacre [path/to/game]\n");
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
        poll_pad(&app);
        app.ff_on = current_core(&app) && !is_paused(&app) &&
                    (app.ff_key || (app.pad_held & PAD_BIT(GBI_FAST_FORWARD)));
        sync_session(&app);
        if (!nacre_session_tick(app.session, elapsed, now)) {
            /* The core halts until F2 or another game; the window stays. */
            const char *fault = nacre_session_error(app.session);
            show_error(&app, fault ? fault : "The game stopped.");
            previous = SDL_GetTicksNS(); /* the dialog's time isn't game time */
        }
        show_save_note(&app);
        audio_out_pump(app.audio, current_core(&app), current_instance(&app),
                       speed_percent[app.speed], !app.muted && !is_paused(&app), app.ff_on);
        if (!render_frame(&app)) {
            fprintf(stderr, "could not render the Nacre framebuffer: %s\n", SDL_GetError());
            app.quit_requested = true;
        }
        SDL_Delay(8);
    }

    cleanup(&app);
    return 0;
}
