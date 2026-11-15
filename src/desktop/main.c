#include "audio.h"
#include "chip8.h"
#include "scheduler.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 960
#define WINDOW_HEIGHT 480
#define AUDIO_CALLBACK_SAMPLES 1024

enum {
    FILE_EVENT_OPENED = 1,
    FILE_EVENT_ERROR = 2
};

typedef struct {
    SDL_AudioStream *stream;
    SDL_AtomicInt active;
    SDL_AtomicInt active_samples;
    Chip8AudioOscillator oscillator;
} DesktopAudio;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *framebuffer;
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
} App;

#define MAX_SCRIPTED_KEYS 64
#define DEFAULT_TEST_CYCLES 300u

typedef struct {
    uint32_t at;
    uint8_t key;
    bool pressed;
} ScriptedKey;

/* Deterministic test mode: run a fixed number of CPU cycles, inject real SDL
 * key events at chosen cycle counts, then capture the rendered window. */
typedef struct {
    bool headless;
    const char *rom_path;
    const char *snapshot_path;
    const char *dump_path;
    uint32_t cycles;
    ScriptedKey keys[MAX_SCRIPTED_KEYS];
    size_t key_count;
} Options;

static const SDL_DialogFileFilter rom_filters[] = {
    { "CHIP-8 ROMs", "ch8;rom;bin" },
    { "All files", "*" }
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
        if (active) SDL_AddAtomicInt(&audio->active_samples, count);
        if (!SDL_PutAudioStreamData(stream, samples,
                                    count * (int)sizeof(samples[0]))) {
            return;
        }
        remaining -= count;
    }
}

static int is_paused(const App *app) {
    return app->user_paused || app->focus_paused;
}

static void sync_audio(App *app) {
    int active = app->audio_available && !app->muted && !is_paused(app) &&
                 app->machine.sound_timer > 0;
    SDL_SetAtomicInt(&app->audio.active, active);
}

static void timer_tick(Chip8 *machine, void *userdata) {
    App *app = (App *)userdata;
    (void)machine;
    sync_audio(app);
}

static int load_rom(App *app, const char *path, char *error,
                    size_t error_capacity) {
    char *owned_path = SDL_strdup(path);
    if (!owned_path) {
        snprintf(error, error_capacity, "not enough memory to remember the ROM path");
        return 0;
    }

    Chip8 candidate;
    chip8_init(&candidate, app->seed);
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (!file) {
        snprintf(error, error_capacity, "cannot open ROM '%s': %s", path, SDL_GetError());
        SDL_free(owned_path);
        return 0;
    }
    Sint64 file_size = SDL_GetIOSize(file);
    if (file_size <= 0 || file_size > CHIP8_MEMORY_SIZE - CHIP8_PROGRAM_START) {
        SDL_CloseIO(file);
        snprintf(error, error_capacity,
                 "ROM is empty, too large, or cannot be measured");
        SDL_free(owned_path);
        return 0;
    }
    uint8_t bytes[CHIP8_MEMORY_SIZE - CHIP8_PROGRAM_START];
    size_t length = (size_t)file_size;
    if (SDL_ReadIO(file, bytes, length) != length) {
        snprintf(error, error_capacity, "could not read ROM '%s'", path);
        SDL_CloseIO(file);
        SDL_free(owned_path);
        return 0;
    }
    SDL_CloseIO(file);
    if (!chip8_load_bytes(&candidate, bytes, length, error, error_capacity)) {
        SDL_free(owned_path);
        return 0;
    }

    SDL_free(app->rom_path);
    app->rom_path = owned_path;
    app->machine = candidate;
    chip8_scheduler_init(&app->scheduler, CHIP8_DEFAULT_CPU_HZ);
    app->user_paused = false;
    sync_audio(app);
    return 1;
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

static SDL_Scancode scancode_for_key(uint8_t key) {
    static const SDL_Scancode map[16] = {
        SDL_SCANCODE_X, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_A,
        SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Z, SDL_SCANCODE_C,
        SDL_SCANCODE_4, SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_V
    };
    return map[key & 0xF];
}

static void release_keys(App *app) {
    for (uint8_t key = 0; key < 16; key++) {
        chip8_set_key(&app->machine, key, 0);
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
    int emulated_key = key_for_scancode(key->scancode);
    if (emulated_key >= 0) {
        chip8_set_key(&app->machine, (uint8_t)emulated_key, pressed);
    }
    if (!pressed || key->repeat) return;

    SDL_Keymod modifiers = key->mod;
    bool command = (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    switch (key->scancode) {
        case SDL_SCANCODE_ESCAPE:
            app->quit_requested = true;
            break;
        case SDL_SCANCODE_SPACE:
            app->user_paused = !app->user_paused;
            sync_audio(app);
            break;
        case SDL_SCANCODE_F2: {
            char error[256];
            if (!load_rom(app, app->rom_path, error, sizeof(error))) {
                show_error(app, error);
            }
            break;
        }
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
        default:
            break;
    }
}

static int draw_frame(App *app) {
    uint8_t pixels[CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT * 4];
    for (size_t i = 0; i < CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT; i++) {
        uint8_t value = app->machine.display[i] ? 255 : 0;
        pixels[i * 4] = value;
        pixels[i * 4 + 1] = value;
        pixels[i * 4 + 2] = value;
        pixels[i * 4 + 3] = 255;
    }

    if (!SDL_UpdateTexture(app->framebuffer, NULL, pixels,
                           CHIP8_DISPLAY_WIDTH * 4)) {
        return 0;
    }

    int width, height;
    if (!SDL_GetRenderOutputSize(app->renderer, &width, &height)) return 0;
    float scale_x = (float)width / CHIP8_DISPLAY_WIDTH;
    float scale_y = (float)height / CHIP8_DISPLAY_HEIGHT;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    SDL_FRect destination = {
        ((float)width - CHIP8_DISPLAY_WIDTH * scale) / 2.0f,
        ((float)height - CHIP8_DISPLAY_HEIGHT * scale) / 2.0f,
        CHIP8_DISPLAY_WIDTH * scale,
        CHIP8_DISPLAY_HEIGHT * scale
    };

    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    if (!SDL_RenderClear(app->renderer) ||
        !SDL_RenderTexture(app->renderer, app->framebuffer, NULL, &destination)) {
        return 0;
    }
    return 1;
}

static int render_frame(App *app) {
    if (!draw_frame(app)) return 0;
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

static int initialize(App *app, const char *rom_path) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "could not initialize SDL: %s\n", SDL_GetError());
        return 0;
    }
    app->sdl_initialized = true;

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
    app->framebuffer = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          CHIP8_DISPLAY_WIDTH,
                                          CHIP8_DISPLAY_HEIGHT);
    if (!app->framebuffer ||
        !SDL_SetTextureScaleMode(app->framebuffer, SDL_SCALEMODE_NEAREST)) {
        fprintf(stderr, "could not create the framebuffer texture: %s\n", SDL_GetError());
        return 0;
    }

    app->rom_event_type = SDL_RegisterEvents(1);
    if (app->rom_event_type == (Uint32)-1) {
        fprintf(stderr, "could not register the ROM event: %s\n", SDL_GetError());
        return 0;
    }

    app->seed = 0x4E414352u;
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

    return 1;
}

static void cleanup(App *app) {
    SDL_SetAtomicInt(&app->audio.active, 0);
    if (app->audio.stream) SDL_DestroyAudioStream(app->audio.stream);
    if (app->framebuffer) SDL_DestroyTexture(app->framebuffer);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    SDL_free(app->rom_path);
    if (app->sdl_initialized) SDL_Quit();
}

static int parse_key_script(Options *options, const char *spec) {
    unsigned key, at, hold;
    if (sscanf(spec, "%x@%u+%u", &key, &at, &hold) != 3 || key > 0xF ||
        hold == 0 || options->key_count + 2 > MAX_SCRIPTED_KEYS) {
        return 0;
    }
    options->keys[options->key_count++] = (ScriptedKey){ at, (uint8_t)key, true };
    options->keys[options->key_count++] = (ScriptedKey){ at + hold, (uint8_t)key, false };
    return 1;
}

static int compare_keys(const void *a, const void *b) {
    const ScriptedKey *left = a, *right = b;
    if (left->at != right->at) return left->at < right->at ? -1 : 1;
    return (int)left->pressed - (int)right->pressed;
}

static void inject_key(App *app, const ScriptedKey *scripted) {
    SDL_Event event;
    SDL_zero(event);
    event.type = scripted->pressed ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode_for_key(scripted->key);
    event.key.down = scripted->pressed;
    SDL_PushEvent(&event);
    while (SDL_PollEvent(&event)) handle_event(app, &event);
}

static int write_dump(const App *app, const char *path) {
    FILE *out = strcmp(path, "-") == 0 ? stdout : fopen(path, "w");
    if (!out) return 0;
    for (int y = 0; y < CHIP8_DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < CHIP8_DISPLAY_WIDTH; x++) {
            fputc(app->machine.display[y * CHIP8_DISPLAY_WIDTH + x] ? '#' : '.', out);
        }
        fputc('\n', out);
    }
    if (out != stdout) fclose(out);
    return 1;
}

static int run_headless(App *app, Options *options) {
    qsort(options->keys, options->key_count, sizeof(options->keys[0]), compare_keys);
    uint32_t done = 0;
    size_t next = 0;
    char error[256];

    for (;;) {
        while (next < options->key_count && options->keys[next].at <= done) {
            inject_key(app, &options->keys[next++]);
        }
        if (done >= options->cycles) break;
        uint32_t target = options->cycles;
        if (next < options->key_count && options->keys[next].at < target) {
            target = options->keys[next].at;
        }
        if (!chip8_scheduler_run_cycles(&app->machine, &app->scheduler,
                                        target - done, timer_tick, app,
                                        error, sizeof(error))) {
            fprintf(stderr, "execution fault after %u cycles: %s\n", done, error);
            return 3;
        }
        done = target;
    }
    sync_audio(app);

    if (!draw_frame(app)) {
        fprintf(stderr, "could not draw the frame: %s\n", SDL_GetError());
        return 4;
    }
    if (options->snapshot_path) {
        SDL_Surface *surface = SDL_RenderReadPixels(app->renderer, NULL);
        if (!surface || !SDL_SavePNG(surface, options->snapshot_path)) {
            fprintf(stderr, "could not write %s: %s\n", options->snapshot_path,
                    SDL_GetError());
            SDL_DestroySurface(surface);
            return 5;
        }
        SDL_DestroySurface(surface);
    }
    SDL_RenderPresent(app->renderer);
    if (options->dump_path && !write_dump(app, options->dump_path)) {
        fprintf(stderr, "could not write %s\n", options->dump_path);
        return 6;
    }

    /* The audio device runs in real time; give it a moment to pull samples. */
    int active = SDL_GetAtomicInt(&app->audio.active);
    if (active) SDL_Delay(150);
    fprintf(stderr,
            "headless ok: cycles=%u pc=0x%03X sound_timer=%u audio_available=%d "
            "audio_active=%d audio_active_samples=%d\n",
            done, app->machine.pc, app->machine.sound_timer,
            app->audio_available, active,
            SDL_GetAtomicInt(&app->audio.active_samples));
    return 0;
}

static int parse_options(int argc, char **argv, Options *options) {
    memset(options, 0, sizeof(*options));
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        bool has_value = i + 1 < argc;
        if (strcmp(arg, "--snapshot") == 0 && has_value) {
            options->snapshot_path = argv[++i];
            options->headless = true;
        } else if (strcmp(arg, "--dump") == 0 && has_value) {
            options->dump_path = argv[++i];
            options->headless = true;
        } else if (strcmp(arg, "--cycles") == 0 && has_value) {
            options->cycles = (uint32_t)strtoul(argv[++i], NULL, 10);
            options->headless = true;
        } else if (strcmp(arg, "--key") == 0 && has_value) {
            if (!parse_key_script(options, argv[++i])) return 0;
            options->headless = true;
        } else if (arg[0] != '-' && !options->rom_path) {
            options->rom_path = arg;
        } else {
            return 0;
        }
    }
    if (options->headless && options->cycles == 0) options->cycles = DEFAULT_TEST_CYCLES;
    return 1;
}

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr,
                "Usage: Nacre [rom.ch8]\n"
                "       Nacre [rom.ch8] [--cycles N] [--key HEX@CYCLE+HOLD]...\n"
                "             [--snapshot out.png] [--dump out.txt|-]\n"
                "Test mode runs N CPU cycles without opening a visible loop,\n"
                "injects keypad events at the given cycle counts, and captures\n"
                "the rendered window and/or the framebuffer.\n");
        return 2;
    }

    App app;
    memset(&app, 0, sizeof(app));
    if (!initialize(&app, options.rom_path)) {
        cleanup(&app);
        return 1;
    }

    if (options.headless) {
        int status = run_headless(&app, &options);
        cleanup(&app);
        return status;
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
        if (!chip8_scheduler_advance(&app.machine, &app.scheduler, elapsed,
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
