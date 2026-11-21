/* The core interface: registry, detection, table validation, and the built-in
 * cores driven only through their NacreCore tables. */

#include "nacre_core.h"
#include "nds_test_rom.h"

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

/* ---- helpers ------------------------------------------------------------ */

static void *start(const NacreCore *core, const uint8_t *data, size_t size) {
    char error[256] = "";
    NacreContent content = {data, size, NULL};
    void *self = core->create(NULL, error, sizeof error);
    if (!self) {
        printf("FAIL create %s: %s\n", core->id, error);
        failures++;
        return NULL;
    }
    if (!core->load(self, &content, error, sizeof error)) {
        printf("FAIL load %s: %s\n", core->id, error);
        failures++;
        core->destroy(self);
        return NULL;
    }
    core->set_running(self, 1);
    return self;
}

/* Advances in 10 ms steps, the way a host does per refresh. */
static int run_ms(const NacreCore *core, void *self, unsigned ms) {
    int ok = 1;
    for (unsigned t = 0; t < ms; t += 10) ok &= core->advance(self, 10 * MS);
    return ok;
}

static int16_t audio[48000 * 2];

/* Reads everything queued; returns frames and whether any was not silent. */
static size_t drain(const NacreCore *core, void *self, int *loud) {
    size_t total = 0, n;
    *loud = 0;
    while ((n = core->audio_read(self, audio, 48000)) > 0) {
        for (size_t i = 0; i < n * 2; i++) *loud |= audio[i] != 0;
        total += n;
    }
    return total;
}

/* ---- CHIP-8 ------------------------------------------------------------- */

/* VA = 60; sound timer = VA; loop. */
static const uint8_t BEEP[] = {0x6A, 0x3C, 0xFA, 0x18, 0x12, 0x04};
/* V0 = 5; I = glyph of V0; draw 5 rows at (V0, V1); loop. */
static const uint8_t DRAW[] = {0x60, 0x05, 0xF0, 0x29, 0xD0, 0x15, 0x12, 0x06};
/* Wait for a key into V1, then beep. */
static const uint8_t WAIT_BEEP[] = {0xF1, 0x0A, 0x6A, 0x3C, 0xFA, 0x18, 0x12, 0x06};
static const uint8_t BAD[] = {0xFF, 0xFF};

static void chip8_audio_checks(void) {
    const NacreCore *core = &nacre_chip8_core;
    int loud;
    void *self = start(core, BEEP, sizeof BEEP);
    if (!self) return;
    CHECK(core->audio_rate(self) == 48000);

    /* Six timer ticks in 100 ms, 800 frames each. */
    CHECK(run_ms(core, self, 100));
    CHECK(drain(core, self, &loud) == 4800);
    CHECK(loud);

    /* The 60-tick timer runs out after a second; then only silence comes. */
    CHECK(run_ms(core, self, 1000));
    drain(core, self, &loud);
    CHECK(run_ms(core, self, 100));
    CHECK(drain(core, self, &loud) == 4800);
    CHECK(!loud);

    /* Stopped: time is discarded, nothing is produced. */
    core->set_running(self, 0);
    CHECK(run_ms(core, self, 100));
    CHECK(drain(core, self, &loud) == 0);

    /* Double speed: twice the emulated time per wall second. */
    core->reset(self);
    core->set_running(self, 1);
    core->set_speed(self, 200);
    CHECK(run_ms(core, self, 100));
    CHECK(drain(core, self, &loud) == 9600);

    /* The queue is bounded: a long run keeps only the newest third of a second. */
    core->set_speed(self, 100);
    CHECK(run_ms(core, self, 1000));
    CHECK(drain(core, self, &loud) == 16384);
    core->destroy(self);
}

static void chip8_video_checks(void) {
    const NacreCore *core = &nacre_chip8_core;
    const NacreVideoFrame *frame;
    void *self = start(core, DRAW, sizeof DRAW);
    uint32_t before;
    int lit = 0;
    if (!self) return;
    frame = core->video(self);
    CHECK(frame && frame->format == NACRE_PIXEL_INDEX8 && frame->levels == 2);
    CHECK(frame && frame->width == 64 && frame->height == 32 && frame->pitch == 64);
    before = frame ? frame->frame_count : 0;
    CHECK(run_ms(core, self, 50));
    frame = core->video(self);
    CHECK(frame && frame->frame_count != before);
    if (frame) {
        const uint8_t *px = (const uint8_t *)frame->pixels;
        for (unsigned i = 0; i < 64 * 32; i++) {
            CHECK(px[i] <= 1);
            lit += px[i];
        }
    }
    CHECK(lit > 0);
    /* An unchanged picture keeps its count. */
    before = frame ? frame->frame_count : 0;
    frame = core->video(self);
    CHECK(frame && frame->frame_count == before);
    core->destroy(self);
}

static void chip8_keypad_checks(void) {
    const NacreCore *core = &nacre_chip8_core;
    int loud;
    void *self = start(core, WAIT_BEEP, sizeof WAIT_BEEP);
    if (!self) return;
    CHECK(run_ms(core, self, 100));
    drain(core, self, &loud);
    CHECK(!loud);
    core->set_keypad(self, 5, 1);
    CHECK(run_ms(core, self, 50));
    core->set_keypad(self, 5, 0);
    CHECK(run_ms(core, self, 100));
    drain(core, self, &loud);
    CHECK(loud);
    core->set_keypad(self, 99, 1); /* out of range is ignored */
    core->destroy(self);
}

static void chip8_fault_checks(void) {
    const NacreCore *core = &nacre_chip8_core;
    void *self = start(core, BAD, sizeof BAD);
    if (!self) return;
    CHECK(core->error(self) == NULL);
    CHECK(!core->advance(self, 10 * MS));
    CHECK(core->error(self) && strstr(core->error(self), "unsupported"));
    CHECK(core->advance(self, 10 * MS)); /* halted, reported once */
    core->reset(self);
    CHECK(core->error(self) == NULL);
    core->destroy(self);

    /* Too large to be a CHIP-8 program. */
    {
        static uint8_t big[4000];
        char error[128] = "";
        NacreContent content = {big, sizeof big, NULL};
        self = core->create(NULL, error, sizeof error);
        CHECK(self && !core->load(self, &content, error, sizeof error) && error[0]);
        if (self) core->destroy(self);
    }
}

static void chip8_state_checks(void) {
    const NacreCore *core = &nacre_chip8_core;
    void *self = start(core, BEEP, sizeof BEEP);
    void *other = start(core, DRAW, sizeof DRAW);
    uint8_t *a, *b;
    size_t size;
    if (!self || !other) return;
    CHECK(run_ms(core, self, 300));
    size = core->state_size(self);
    CHECK(size > 0);
    a = (uint8_t *)malloc(size);
    b = (uint8_t *)malloc(size);
    CHECK(core->state_save(self, a, size) == size);
    CHECK(core->state_save(self, b, size - 1) == 0);
    CHECK(run_ms(core, self, 300));
    CHECK(core->state_load(self, a, size));
    CHECK(core->state_save(self, b, size) == size);
    CHECK(memcmp(a, b, size) == 0);
    /* Wrong program, truncated, or junk: refused. */
    CHECK(!core->state_load(other, a, size));
    CHECK(!core->state_load(self, a, size - 1));
    memset(b, 0xA5, size);
    CHECK(!core->state_load(self, b, size));

    CHECK(core->set_option(self, "cpu_hz", "1000"));
    CHECK(!core->set_option(self, "cpu_hz", "fast"));
    CHECK(!core->set_option(self, "cpu_hz", "0"));
    CHECK(!core->set_option(self, "volume", "3"));
    CHECK(core->title(self) == NULL);
    free(a);
    free(b);
    core->destroy(self);
    core->destroy(other);
}

/* ---- Game Boy ----------------------------------------------------------- */

static uint8_t gb_rom[0x8000];

static void make_gb_rom(void) {
    static const uint8_t logo[48] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    };
    memcpy(gb_rom + 0x104, logo, sizeof logo);
    memcpy(gb_rom + 0x134, "NACRETEST", 9);
    gb_rom[0x100] = 0xC3; gb_rom[0x101] = 0x50; gb_rom[0x102] = 0x01; /* JP $0150 */
    gb_rom[0x150] = 0x18; gb_rom[0x151] = 0xFE;                       /* JR -2 */
}

static void gb_checks(void) {
    const NacreCore *core = &nacre_gb_core;
    const NacreVideoFrame *frame;
    int loud;
    size_t frames, size;
    uint8_t *a, *b;
    void *self = start(core, gb_rom, sizeof gb_rom);
    if (!self) return;
    CHECK(core->title(self) && !strcmp(core->title(self), "NACRETEST"));
    CHECK(core->save_size(self) == 0); /* no battery on this cart */
    CHECK(core->audio_rate(self) == 48000);

    frame = core->video(self);
    CHECK(frame && frame->format == NACRE_PIXEL_INDEX8 && frame->levels == 4);
    CHECK(frame && frame->width == 160 && frame->height == 144 && frame->pitch == 160);
    CHECK(run_ms(core, self, 100));
    frame = core->video(self);
    CHECK(frame && frame->frame_count >= 5);
    frames = drain(core, self, &loud);
    CHECK(frames >= 4700 && frames <= 4900);

    core->set_pad(self, 0, NACRE_PAD_BIT(NACRE_PAD_START) | NACRE_PAD_BIT(NACRE_PAD_EAST));
    CHECK(run_ms(core, self, 20));
    core->set_pad(self, 0, 0);
    core->set_pad(self, 1, 0xFFFF); /* second port: nothing there */

    size = core->state_size(self);
    CHECK(size > 0);
    a = (uint8_t *)malloc(size);
    b = (uint8_t *)malloc(size);
    CHECK(core->state_save(self, a, size) == size);
    frame = core->video(self);
    {
        /* Byte-exact rewinds are test_gb's job (the state carries the audio queue);
         * here the picture must go back to where it was. */
        uint32_t saved = frame ? frame->frame_count : 0;
        CHECK(run_ms(core, self, 100));
        CHECK(core->state_load(self, a, size));
        frame = core->video(self);
        CHECK(frame && frame->frame_count == saved);
        CHECK(core->advance(self, 10 * MS));
    }
    memset(b, 0, size);
    CHECK(!core->state_load(self, b, size));
    free(a);
    free(b);

    /* Stopped: frames hold still. */
    core->set_running(self, 0);
    frame = core->video(self);
    {
        uint32_t held = frame ? frame->frame_count : 0;
        CHECK(run_ms(core, self, 100));
        frame = core->video(self);
        CHECK(frame && frame->frame_count == held);
    }
    core->destroy(self);
}

/* ---- Nintendo DS -------------------------------------------------------- */

static uint8_t nds_rom[NDS_TEST_ROM_SIZE];

static void nds_checks(void) {
    const NacreCore *core = &nacre_nds_core;
    const NacreVideoFrame *frame;
    int loud;
    size_t frames;
    void *self = start(core, nds_rom, sizeof nds_rom);
    if (!self) return;
    CHECK(core->title(self) && !strcmp(core->title(self), "Nacre Test"));
    CHECK(core->audio_rate(self) == 48000);
    CHECK(core->pad_labels[NACRE_PAD_NORTH] && !strcmp(core->pad_labels[NACRE_PAD_NORTH], "X"));
    CHECK(core->pad_labels[NACRE_PAD_L1] && core->pad_labels[NACRE_PAD_R1]);

    frame = core->video(self);
    CHECK(frame && frame->format == NACRE_PIXEL_XRGB8888);
    CHECK(frame && frame->width == 256 && frame->height == 384 && frame->pitch == 1024);
    CHECK(run_ms(core, self, 100));
    frame = core->video(self);
    CHECK(frame && frame->frame_count >= 5 && frame->frame_count <= 6);
    frames = drain(core, self, &loud);
    CHECK(frames >= 4700 && frames <= 4900);

    core->set_pad(self, 0, NACRE_PAD_BIT(NACRE_PAD_NORTH) | NACRE_PAD_BIT(NACRE_PAD_L1));
    core->set_pointer(self, 0, 100, 300, 1);
    core->set_pointer(self, 0, 100, 50, 1); /* top screen: not a touch */
    core->set_pointer(self, 0, -1, -1, 0);
    CHECK(run_ms(core, self, 20));
    core->set_pad(self, 0, 0);
    core->destroy(self);
}

/* ---- registry ----------------------------------------------------------- */

/* A stand-in for a forked emulator: its own threads, its own window, one at a time. */
static void *mock_create(const NacreHost *h, char *e, size_t c) { (void)h; (void)e; (void)c; return (void *)1; }
static void mock_destroy(void *s) { (void)s; }
static int mock_load(void *s, const NacreContent *ct, char *e, size_t c) { (void)s; (void)ct; (void)e; (void)c; return 1; }
static void mock_void(void *s) { (void)s; }
static void mock_int(void *s, int v) { (void)s; (void)v; }
static void mock_speed(void *s, unsigned v) { (void)s; (void)v; }
static const char *mock_error(void *s) { (void)s; return NULL; }
static int mock_attach(void *s, const NacreSurface *sf, char *e, size_t c) { (void)s; (void)sf; (void)e; (void)c; return 1; }
static unsigned mock_rate(void *s) { (void)s; return 48000; }
static size_t mock_read(void *s, int16_t *o, size_t m) { (void)s; (void)o; (void)m; return 0; }
static void mock_pad(void *s, unsigned p, uint32_t b) { (void)s; (void)p; (void)b; }
static int mock_probe(const uint8_t *h, size_t hs, size_t s, const char *p) { (void)h; (void)hs; (void)s; (void)p; return 0; }

static NacreCore mock_threaded(void) {
    NacreCore core;
    memset(&core, 0, sizeof core);
    core.api_version = NACRE_CORE_API_VERSION;
    core.id = "mock";
    core.name = "Mock";
    core.flags = NACRE_CORE_THREADED | NACRE_CORE_NATIVE_VIDEO | NACRE_CORE_SINGLETON |
                 NACRE_CORE_NEEDS_PATH | NACRE_CORE_NEEDS_SYSTEM_FILES;
    core.pad_labels[NACRE_PAD_SOUTH] = "Cross";
    core.probe = mock_probe;
    core.create = mock_create;
    core.destroy = mock_destroy;
    core.load = mock_load;
    core.reset = mock_void;
    core.set_running = mock_int;
    core.set_speed = mock_speed;
    core.error = mock_error;
    core.attach_surface = mock_attach;
    core.audio_rate = mock_rate;
    core.audio_read = mock_read;
    core.audio_clear = mock_void;
    core.set_pad = mock_pad;
    return core;
}

static int mock_advance(void *s, uint64_t ns) { (void)s; (void)ns; return 1; }

static void registry_checks(void) {
    char error[256];
    const NacreCore *const *cores = nacre_cores();
    static uint8_t big[0x8000];
    static uint8_t head[0x4000];
    NacreCore mock;
    int count = 0;

    for (; cores[count]; count++) {
        if (!nacre_core_validate(cores[count], error, sizeof error)) {
            printf("FAIL built-in core invalid: %s\n", error);
            failures++;
        }
        CHECK(nacre_core_by_id(cores[count]->id) == cores[count]);
    }
    CHECK(count == 3);
    CHECK(nacre_core_by_id("ps2") == NULL);
    CHECK(nacre_core_by_id(NULL) == NULL);

    CHECK(nacre_core_detect(gb_rom, sizeof gb_rom, sizeof gb_rom, NULL) == &nacre_gb_core);
    CHECK(nacre_core_detect(gb_rom, sizeof gb_rom, sizeof gb_rom, "x.ch8") == &nacre_gb_core);
    CHECK(nacre_core_detect(BEEP, sizeof BEEP, sizeof BEEP, NULL) == &nacre_chip8_core);
    CHECK(nacre_core_detect(BEEP, sizeof BEEP, sizeof BEEP, "beep.ch8") == &nacre_chip8_core);
    CHECK(nacre_core_detect(nds_rom, sizeof nds_rom, sizeof nds_rom, NULL) == &nacre_nds_core);
    CHECK(nacre_core_detect(nds_rom, 0x200, 64u << 20, "game.nds") == &nacre_nds_core);
    CHECK(nacre_core_detect(big, sizeof big, sizeof big, NULL) == NULL);
    CHECK(nacre_core_detect(NULL, 0, 0, NULL) == NULL);
    /* A large file is probed from its head alone. */
    memcpy(head, gb_rom, sizeof head);
    CHECK(nacre_core_detect(head, sizeof head, 4u << 20, "big.gb") == &nacre_gb_core);

    /* The table can describe a threaded, native-video core. */
    mock = mock_threaded();
    CHECK(nacre_core_validate(&mock, error, sizeof error));
    mock.attach_surface = NULL;
    CHECK(!nacre_core_validate(&mock, error, sizeof error) && strstr(error, "attach_surface"));
    mock = mock_threaded();
    mock.advance = mock_advance;
    CHECK(!nacre_core_validate(&mock, error, sizeof error) && strstr(error, "advance"));
    mock = mock_threaded();
    mock.set_pad = NULL;
    CHECK(!nacre_core_validate(&mock, error, sizeof error) && strstr(error, "set_pad"));
    mock = mock_threaded();
    mock.flags |= NACRE_CORE_HAS_STATES;
    CHECK(!nacre_core_validate(&mock, error, sizeof error) && strstr(error, "state"));
    mock = mock_threaded();
    mock.flags |= NACRE_CORE_HAS_POINTER;
    CHECK(!nacre_core_validate(&mock, error, sizeof error) && strstr(error, "set_pointer"));
    mock = mock_threaded();
    mock.flags |= NACRE_CORE_SAVE_SIZED_LATER;
    CHECK(!nacre_core_validate(&mock, error, sizeof error) && strstr(error, "battery"));
    mock = mock_threaded();
    mock.api_version = NACRE_CORE_API_VERSION + 1;
    CHECK(!nacre_core_validate(&mock, error, sizeof error));
    CHECK(!nacre_core_validate(NULL, error, sizeof error));
}

int main(void) {
    make_gb_rom();
    nds_test_make_rom(nds_rom, "Nacre Test\nSecond line");
    registry_checks();
    chip8_audio_checks();
    chip8_video_checks();
    chip8_keypad_checks();
    chip8_fault_checks();
    chip8_state_checks();
    gb_checks();
    nds_checks();
    if (failures) {
        printf("test_nacre_core: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_nacre_core: ok\n");
    return 0;
}
