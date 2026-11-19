/* The session layer against an in-memory file system: core selection and
 * rollback, battery saves and their debounce, state slots, options, faults, and
 * single-instance cores. */

#include "nacre_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

#define MS 1000000ull

/* ---- in-memory files ---------------------------------------------------- */

#define MAX_FILES 32

typedef struct {
    char path[128];
    uint8_t *data;
    size_t size;
} File;

static File files[MAX_FILES];
static int writes;
static int fail_writes;

static File *find(const char *path) {
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].data && !strcmp(files[i].path, path)) return &files[i];
    return NULL;
}

static void put(const char *path, const uint8_t *data, size_t size) {
    File *f = find(path);
    for (int i = 0; !f && i < MAX_FILES; i++)
        if (!files[i].data) f = &files[i];
    if (!f) return;
    free(f->data);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->data = (uint8_t *)malloc(size ? size : 1);
    memcpy(f->data, data, size);
    f->size = size;
}

static void wipe(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        free(files[i].data);
        files[i].data = NULL;
    }
    writes = 0;
    fail_writes = 0;
}

static NacreIoResult io_read(void *ud, const char *path, size_t max, uint8_t **data, size_t *size) {
    File *f = find(path);
    (void)ud;
    if (!f) return NACRE_IO_MISSING;
    if (f->size > max) return NACRE_IO_FAILED;
    *data = (uint8_t *)malloc(f->size ? f->size : 1);
    memcpy(*data, f->data, f->size);
    *size = f->size;
    return NACRE_IO_OK;
}

static int io_write(void *ud, const char *path, const uint8_t *data, size_t size) {
    (void)ud;
    if (fail_writes) return 0;
    put(path, data, size);
    writes++;
    return 1;
}

static int64_t io_now(void *ud) {
    (void)ud;
    return 1700000000;
}

static const NacreSessionIo IO = {io_read, io_write, io_now, NULL};

/* ---- content ------------------------------------------------------------ */

static const uint8_t BEEP[] = {0x6A, 0x3C, 0xFA, 0x18, 0x12, 0x04};
static const uint8_t BAD[] = {0xFF, 0xFF};
static uint8_t gb_rom[0x8000];

/* MBC3 + RAM + battery; the program keeps writing cartridge RAM. */
static void make_gb_rom(void) {
    static const uint8_t logo[48] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    };
    static const uint8_t program[] = {
        0x3E, 0x0A, 0xEA, 0x00, 0x00, /* enable cartridge RAM */
        0x04, 0x78,                   /* loop: INC B; LD A,B */
        0xEA, 0x00, 0xA0,             /* LD ($A000),A */
        0x18, 0xF9,                   /* JR loop */
    };
    memcpy(gb_rom + 0x104, logo, sizeof logo);
    memcpy(gb_rom + 0x134, "SAVETEST", 8);
    gb_rom[0x147] = 0x13;
    gb_rom[0x149] = 0x03;
    gb_rom[0x100] = 0xC3; gb_rom[0x101] = 0x50; gb_rom[0x102] = 0x01;
    memcpy(gb_rom + 0x150, program, sizeof program);
}

static NacreSession *session_new(int persist) { return nacre_session_create(&IO, NULL, persist); }

static uint64_t clock_ns;

static int tick_ms(NacreSession *s, unsigned ms) {
    int ok = 1;
    for (unsigned t = 0; t < ms; t += 10) {
        clock_ns += 10 * MS;
        ok &= nacre_session_tick(s, 10 * MS, clock_ns);
    }
    return ok;
}

static int loud(NacreSession *s) {
    static int16_t buf[48000 * 2];
    const NacreCore *core = nacre_session_core(s);
    size_t n;
    int any = 0;
    while ((n = core->audio_read(nacre_session_instance(s), buf, 48000)) > 0)
        for (size_t i = 0; i < n * 2; i++) any |= buf[i] != 0;
    return any;
}

/* ---- checks ------------------------------------------------------------- */

static void save_checks(void) {
    char error[256];
    NacreSession *s;
    File *sav;
    int before;

    wipe();
    s = session_new(1);
    CHECK(nacre_session_core(s) == NULL && nacre_session_error(s) == NULL);
    CHECK(nacre_session_tick(s, 10 * MS, 0)); /* empty is fine */
    CHECK(nacre_session_open(s, NULL, "C:\\roms\\Save Test.gb", gb_rom, sizeof gb_rom, error, sizeof error));
    CHECK(nacre_session_core(s) == &nacre_gb_core);
    CHECK(!strcmp(nacre_session_save_note(s), "Saves to Save Test.sav"));

    /* Games write their RAM in bursts, so the save goes out a second after the
     * first change, not on every write. */
    CHECK(tick_ms(s, 500));
    CHECK(find("C:\\roms\\Save Test.sav") == NULL);
    CHECK(tick_ms(s, 1000));
    sav = find("C:\\roms\\Save Test.sav");
    CHECK(sav && sav->size == 32768);
    CHECK(!strcmp(nacre_session_save_note(s), "Saved to Save Test.sav"));

    /* Nothing changes while stopped, so nothing is written. */
    nacre_session_flush(s);
    nacre_session_set_running(s, 0);
    before = writes;
    CHECK(tick_ms(s, 2000));
    CHECK(writes == before);
    nacre_session_set_running(s, 1);

    /* A failed write is reported and retried later. */
    CHECK(tick_ms(s, 50));
    fail_writes = 1;
    nacre_session_flush(s);
    CHECK(!strcmp(nacre_session_save_note(s), "Couldn't write Save Test.sav"));
    fail_writes = 0;

    /* Switching games writes the old one's save first. */
    before = writes;
    CHECK(nacre_session_open(s, NULL, "C:\\roms\\beep.ch8", BEEP, sizeof BEEP, error, sizeof error));
    CHECK(writes == before + 1);
    CHECK(nacre_session_core(s) == &nacre_chip8_core);
    CHECK(!strcmp(nacre_session_save_note(s), ""));
    nacre_session_destroy(s);

    /* A new session finds the save. */
    s = session_new(1);
    CHECK(nacre_session_open(s, NULL, "C:\\roms\\Save Test.gb", gb_rom, sizeof gb_rom, error, sizeof error));
    CHECK(!strcmp(nacre_session_save_note(s), "Loaded Save Test.sav"));
    nacre_session_destroy(s);

    /* One that doesn't fit is left alone, never overwritten. */
    put("/games/Save Test.sav", (const uint8_t *)"junk!", 5);
    s = session_new(1);
    CHECK(nacre_session_open(s, NULL, "/games/Save Test.gb", gb_rom, sizeof gb_rom, error, sizeof error));
    CHECK(strstr(nacre_session_save_note(s), "Ignored Save Test.sav") != NULL);
    CHECK(tick_ms(s, 100));
    nacre_session_destroy(s);
    CHECK(find("/games/Save Test.sav")->size == 5);

    /* Without persistence nothing is written at all. */
    wipe();
    s = session_new(0);
    CHECK(nacre_session_open(s, NULL, "/x/Save Test.gb", gb_rom, sizeof gb_rom, error, sizeof error));
    CHECK(!strcmp(nacre_session_save_note(s), "Not saving Save Test.sav this run"));
    CHECK(tick_ms(s, 100));
    CHECK(!nacre_session_save_state(s, 0, error, sizeof error));
    nacre_session_destroy(s);
    CHECK(writes == 0);
}

static void rollback_checks(void) {
    char error[256];
    static uint8_t junk[8192];
    static uint8_t big[4000];
    NacreSession *s = session_new(1);
    wipe();
    CHECK(nacre_session_open(s, NULL, "/r/a.gb", gb_rom, sizeof gb_rom, error, sizeof error));
    CHECK(!nacre_session_open(s, NULL, "/r/junk.bin", junk, sizeof junk, error, sizeof error));
    CHECK(strstr(error, "recognize") != NULL);
    CHECK(!nacre_session_open(s, nacre_core_by_id("chip8"), "/r/big.ch8", big, sizeof big, error, sizeof error));
    CHECK(nacre_session_core(s) == &nacre_gb_core);
    CHECK(!strcmp(nacre_session_path(s), "/r/a.gb"));

    /* Built-in content: no path, no states. */
    CHECK(nacre_session_open(s, nacre_core_by_id("chip8"), NULL, BEEP, sizeof BEEP, error, sizeof error));
    CHECK(nacre_session_path(s) == NULL);
    CHECK(!nacre_session_has_states(s));
    CHECK(!nacre_session_save_state(s, 0, error, sizeof error) && strstr(error, "can't keep"));
    nacre_session_close(s);
    CHECK(nacre_session_core(s) == NULL);
    nacre_session_destroy(s);
}

static void state_checks(void) {
    char error[256], path[256];
    NacreSession *s = session_new(1);
    File *gb_state;
    wipe();
    CHECK(nacre_session_open(s, NULL, "/r/beep.ch8", BEEP, sizeof BEEP, error, sizeof error));
    CHECK(nacre_session_has_states(s));
    CHECK(nacre_session_state_path(s, 1, path, sizeof path) && !strcmp(path, "/r/beep.state2"));
    CHECK(!nacre_session_state_path(s, NACRE_STATE_SLOTS, path, sizeof path));
    CHECK(!nacre_session_state_path(s, 0, path, 5));
    CHECK(!nacre_session_load_state(s, 0, error, sizeof error) && !strcmp(error, "Slot 1 is empty"));

    CHECK(tick_ms(s, 200));
    CHECK(nacre_session_save_state(s, 0, error, sizeof error));
    CHECK(find("/r/beep.state1") != NULL);
    CHECK(tick_ms(s, 1500)); /* the beep has ended */
    loud(s);
    CHECK(nacre_session_load_state(s, 0, error, sizeof error));
    CHECK(tick_ms(s, 100));
    CHECK(loud(s)); /* back inside the beep */

    /* Another system's state is refused. */
    CHECK(nacre_session_open(s, NULL, "/r/a.gb", gb_rom, sizeof gb_rom, error, sizeof error));
    CHECK(tick_ms(s, 50));
    CHECK(nacre_session_save_state(s, 3, error, sizeof error));
    gb_state = find("/r/a.state4");
    CHECK(gb_state != NULL);
    if (gb_state) put("/r/beep.state2", gb_state->data, gb_state->size);
    CHECK(nacre_session_open(s, NULL, "/r/beep.ch8", BEEP, sizeof BEEP, error, sizeof error));
    CHECK(!nacre_session_load_state(s, 1, error, sizeof error) && !strcmp(error, "Slot 2 doesn't fit this game"));
    nacre_session_destroy(s);
}

static void option_and_run_checks(void) {
    char error[256];
    NacreSession *s = session_new(1);
    wipe();
    /* Remembered while another core runs, applied when CHIP-8 starts: at 1 Hz the
     * beep program hasn't reached its second instruction after 100 ms. */
    CHECK(nacre_session_open(s, NULL, "/r/a.gb", gb_rom, sizeof gb_rom, error, sizeof error));
    CHECK(nacre_session_set_option(s, "chip8", "cpu_hz", "1"));
    CHECK(nacre_session_open(s, NULL, "/r/beep.ch8", BEEP, sizeof BEEP, error, sizeof error));
    CHECK(tick_ms(s, 100));
    CHECK(!loud(s));
    CHECK(nacre_session_set_option(s, "chip8", "cpu_hz", "600"));
    CHECK(!nacre_session_set_option(s, "chip8", "cpu_hz", "bogus"));
    CHECK(!nacre_session_set_option(s, "chip8", "volume", "3")); /* the running core refuses it */
    CHECK(tick_ms(s, 100));
    CHECK(loud(s));

    /* Paused before opening: the new game waits. */
    nacre_session_set_running(s, 0);
    CHECK(nacre_session_open(s, NULL, "/r/beep2.ch8", BEEP, sizeof BEEP, error, sizeof error));
    CHECK(tick_ms(s, 100));
    CHECK(!loud(s));
    nacre_session_set_running(s, 1);
    CHECK(tick_ms(s, 100));
    CHECK(loud(s));

    /* Faults are reported once, and a restart clears them. */
    CHECK(nacre_session_open(s, NULL, "/r/bad.ch8", BAD, sizeof BAD, error, sizeof error));
    CHECK(!nacre_session_tick(s, 10 * MS, clock_ns));
    CHECK(nacre_session_error(s) != NULL);
    CHECK(nacre_session_tick(s, 10 * MS, clock_ns));
    nacre_session_restart(s);
    CHECK(nacre_session_error(s) == NULL);
    CHECK(!nacre_session_tick(s, 10 * MS, clock_ns));
    nacre_session_destroy(s);
}

/* ---- a single-instance threaded core ------------------------------------ */

typedef struct {
    int fail_load;
    const char *fault;
    unsigned surface_width; /* 0 while detached */
    int ran_detached;
} Mock;

static int live;

static void *mock_create(const NacreHost *h, char *e, size_t c) {
    (void)h;
    if (live) {
        snprintf(e, c, "already running");
        return NULL;
    }
    live++;
    return calloc(1, sizeof(Mock));
}
static void mock_destroy(void *s) {
    live--;
    free(s);
}
static int mock_load(void *s, const NacreContent *ct, char *e, size_t c) {
    (void)s;
    if (!ct->path || strstr(ct->path, "broken")) {
        snprintf(e, c, "bad disc");
        return 0;
    }
    return 1;
}
static void mock_void(void *s) { ((Mock *)s)->fault = NULL; }
static void mock_audio_clear(void *s) { (void)s; }
static void mock_speed(void *s, unsigned v) { (void)s; (void)v; }
static const char *mock_error(void *s) { return ((Mock *)s)->fault; }
static int refuse_surface;
static int mock_attach(void *s, const NacreSurface *sf, char *e, size_t c) {
    if (refuse_surface) {
        snprintf(e, c, "no swap chain");
        return 0;
    }
    ((Mock *)s)->surface_width = sf ? sf->width : 0;
    return 1;
}
static void mock_running(void *s, int v) {
    if (v && !((Mock *)s)->surface_width) ((Mock *)s)->ran_detached = 1;
}
static unsigned mock_rate(void *s) { (void)s; return 48000; }
static size_t mock_read(void *s, int16_t *o, size_t m) { (void)s; (void)o; (void)m; return 0; }
static int mock_probe(const uint8_t *h, size_t hs, size_t s, const char *p) { (void)h; (void)hs; (void)s; (void)p; return 0; }

static NacreCore mock_core(void) {
    NacreCore core;
    memset(&core, 0, sizeof core);
    core.api_version = NACRE_CORE_API_VERSION;
    core.id = "mock";
    core.name = "Mock";
    core.flags = NACRE_CORE_THREADED | NACRE_CORE_NATIVE_VIDEO | NACRE_CORE_SINGLETON | NACRE_CORE_NEEDS_PATH;
    core.probe = mock_probe;
    core.create = mock_create;
    core.destroy = mock_destroy;
    core.load = mock_load;
    core.reset = mock_void;
    core.set_running = mock_running;
    core.set_speed = mock_speed;
    core.error = mock_error;
    core.attach_surface = mock_attach;
    core.audio_rate = mock_rate;
    core.audio_read = mock_read;
    core.audio_clear = mock_audio_clear;
    return core;
}

static void singleton_checks(void) {
    char error[256];
    NacreCore mock = mock_core();
    NacreSurface surface = {NACRE_SURFACE_WIN32_HWND, &surface, NULL, 640, 480, 1.0f};
    NacreSession *s = session_new(1);
    wipe();
    CHECK(!nacre_session_open(s, &mock, NULL, NULL, 0, error, sizeof error) && strstr(error, "from a file"));
    /* A native-video core needs somewhere to draw, given before it first runs. */
    CHECK(!nacre_session_open(s, &mock, "/d/one.iso", NULL, 0, error, sizeof error) &&
          strstr(error, "window") && live == 0);
    CHECK(nacre_session_set_surface(s, &surface, error, sizeof error));
    refuse_surface = 1;
    CHECK(!nacre_session_open(s, &mock, "/d/one.iso", NULL, 0, error, sizeof error) &&
          !strcmp(error, "no swap chain") && live == 0);
    refuse_surface = 0;
    CHECK(nacre_session_open(s, &mock, "/d/one.iso", NULL, 0, error, sizeof error));
    CHECK(live == 1);
    CHECK(((Mock *)nacre_session_instance(s))->surface_width == 640);
    CHECK(!((Mock *)nacre_session_instance(s))->ran_detached);
    /* A resize reaches the open game. */
    surface.width = 800;
    CHECK(nacre_session_set_surface(s, &surface, error, sizeof error));
    CHECK(((Mock *)nacre_session_instance(s))->surface_width == 800);
    /* The old instance is shut down before the new one is made. */
    CHECK(nacre_session_open(s, &mock, "/d/two.iso", NULL, 0, error, sizeof error));
    CHECK(live == 1);
    CHECK(!strcmp(nacre_session_path(s), "/d/two.iso"));
    /* A threaded core's fault is noticed on the next tick, once. */
    ((Mock *)nacre_session_instance(s))->fault = "the emulator stopped";
    CHECK(!nacre_session_tick(s, 10 * MS, clock_ns));
    CHECK(nacre_session_tick(s, 10 * MS, clock_ns));
    nacre_session_restart(s);
    CHECK(nacre_session_error(s) == NULL);
    /* Which is why a failed load leaves a single-instance session empty. */
    CHECK(!nacre_session_open(s, &mock, "/d/broken.iso", NULL, 0, error, sizeof error));
    CHECK(!strcmp(error, "bad disc"));
    CHECK(nacre_session_core(s) == NULL && live == 0);
    /* Other cores are untouched by it. */
    CHECK(nacre_session_open(s, NULL, "/r/beep.ch8", BEEP, sizeof BEEP, error, sizeof error));
    CHECK(nacre_session_open(s, &mock, "/d/one.iso", NULL, 0, error, sizeof error));
    CHECK(!nacre_session_open(s, &mock, "/d/broken.iso", NULL, 0, error, sizeof error));
    nacre_session_destroy(s);
    CHECK(live == 0);
}

int main(void) {
    make_gb_rom();
    save_checks();
    rollback_checks();
    state_checks();
    option_and_run_checks();
    singleton_checks();
    wipe();
    if (failures) {
        printf("test_nacre_session: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_nacre_session: ok\n");
    return 0;
}
