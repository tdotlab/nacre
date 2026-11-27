/* The DS core through its own API: cartridge header checks, timing, audio pacing,
 * input, and, given their directory, the test ROMs that tools/get-nds-test-roms.py
 * builds. With --run it loads a cartridge from disk and runs it headless, with
 * scripted presses, frame hashes and dumps (see usage()).
 */

#include "nds.h"
#include "nds_test_rom.h"
#include "../src/nds/nds_internal.h" /* machine_checks look inside */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

static uint8_t rom[NDS_TEST_ROM_SIZE];
static char error[256];

static int loads(const uint8_t *data, size_t size) {
    Nds *nds = nds_create();
    int ok = nds_load_rom(nds, data, size, error, sizeof error);
    nds_destroy(nds);
    return ok;
}

static void crc_checks(void) {
    /* The standard check value for this CRC (init 0xFFFF, reflected 0xA001). */
    CHECK(nds_crc16(0xFFFF, (const uint8_t *)"123456789", 9) == 0x4B37);
    CHECK(nds_crc16(0xFFFF, NULL, 0) == 0xFFFF);
}

static void header_checks(void) {
    static uint8_t bad[NDS_TEST_ROM_SIZE];
    Nds *nds;

    nds_test_make_rom(rom, "Nacre Test\nSecond line");
    CHECK(nds_rom_looks_valid(rom, sizeof rom));
    CHECK(nds_rom_looks_valid(rom, 0x160));
    CHECK(!nds_rom_looks_valid(rom, 0x15F));
    CHECK(!nds_rom_looks_valid(NULL, 0));

    nds = nds_create();
    CHECK(!nds_has_rom(nds));
    CHECK(!strcmp(nds_title(nds), "") && !strcmp(nds_game_code(nds), ""));
    CHECK(nds_load_rom(nds, rom, sizeof rom, error, sizeof error));
    CHECK(nds_has_rom(nds));
    CHECK(!strcmp(nds_title(nds), "Nacre Test"));
    CHECK(!strcmp(nds_game_code(nds), "NTST"));
    nds_destroy(nds);

    /* No banner: the header title stands in. */
    nds_test_make_rom(rom, NULL);
    nds = nds_create();
    CHECK(nds_load_rom(nds, rom, sizeof rom, error, sizeof error));
    CHECK(!strcmp(nds_title(nds), "NACRETEST"));
    nds_destroy(nds);

    /* A flipped header byte breaks the header checksum. */
    memcpy(bad, rom, sizeof bad);
    bad[0x00] ^= 1;
    CHECK(!nds_rom_looks_valid(bad, sizeof bad));
    CHECK(!loads(bad, sizeof bad) && strstr(error, "valid DS cartridge header"));

    /* So does a broken logo field, even with the header resealed. */
    memcpy(bad, rom, sizeof bad);
    bad[0xC0] ^= 1;
    nds_test_seal(bad);
    CHECK(!nds_rom_looks_valid(bad, sizeof bad));

    /* Homebrew with its own logo loads when the logo's checksum field matches. */
    {
        uint16_t logo = nds_crc16(0xFFFF, bad + 0xC0, 0x9C);
        bad[0x15C] = (uint8_t)logo;
        bad[0x15D] = (uint8_t)(logo >> 8);
        nds_test_seal(bad);
        CHECK(nds_rom_looks_valid(bad, sizeof bad) && loads(bad, sizeof bad));
    }

    /* DSi-only cartridges are refused by name. */
    memcpy(bad, rom, sizeof bad);
    bad[0x12] = 0x03;
    nds_test_seal(bad);
    CHECK(!loads(bad, sizeof bad) && strstr(error, "DSi-only"));

    /* An ARM9 program past the end of the file. */
    memcpy(bad, rom, sizeof bad);
    nds_test_put32(bad + 0x20, NDS_TEST_ROM_SIZE - 0x800);
    nds_test_seal(bad);
    CHECK(!loads(bad, sizeof bad) && strstr(error, "ARM9"));

    /* An ARM7 program aimed outside main RAM and its own RAM. */
    memcpy(bad, rom, sizeof bad);
    nds_test_put32(bad + 0x34, 0x06000000);
    nds_test_put32(bad + 0x38, 0x06000000);
    nds_test_seal(bad);
    CHECK(!loads(bad, sizeof bad) && strstr(error, "ARM7"));

    /* An entry point outside its program. */
    memcpy(bad, rom, sizeof bad);
    nds_test_put32(bad + 0x24, 0x02001000);
    nds_test_seal(bad);
    CHECK(!loads(bad, sizeof bad) && strstr(error, "ARM9"));

    CHECK(!loads(rom, 0x100) && strstr(error, "too small"));

    /* A failed load keeps the cartridge that was there. */
    nds = nds_create();
    CHECK(nds_load_rom(nds, rom, sizeof rom, error, sizeof error));
    memcpy(bad, rom, sizeof bad);
    bad[0] ^= 1;
    CHECK(!nds_load_rom(nds, bad, sizeof bad, error, sizeof error));
    CHECK(nds_has_rom(nds) && !strcmp(nds_game_code(nds), "NTST"));
    nds_destroy(nds);
}

static void timing_checks(void) {
    static int16_t audio[NDS_AUDIO_SAMPLE_RATE * 2];
    Nds *nds = nds_create();
    size_t total = 0, n;
    nds_test_make_rom(rom, NULL);
    CHECK(nds_load_rom(nds, rom, sizeof rom, error, sizeof error));

    /* One frame is exactly NDS_CYCLES_PER_FRAME. */
    CHECK(nds_run_cycles(nds, NDS_CYCLES_PER_FRAME - 1, error, sizeof error));
    CHECK(nds_frame_count(nds) == 0);
    CHECK(nds_run_cycles(nds, 1, error, sizeof error));
    CHECK(nds_frame_count(nds) == 1);
    nds_reset(nds);
    CHECK(nds_frame_count(nds) == 0);

    /* One emulated second, in 10 ms host steps: about 59.83 frames and exactly
     * 48000 audio frames, read as it goes the way a host would. */
    for (int i = 0; i < 100; i++) {
        CHECK(nds_advance_ns(nds, 10000000u, 100, error, sizeof error));
        while ((n = nds_audio_read(nds, audio, NDS_AUDIO_SAMPLE_RATE)) > 0) total += n;
    }
    CHECK(nds_frame_count(nds) == 59);
    CHECK(total == NDS_AUDIO_SAMPLE_RATE);

    /* Double speed runs twice the frames; a huge stall is capped at 250 ms. */
    nds_reset(nds);
    CHECK(nds_advance_ns(nds, 100000000u, 200, error, sizeof error));
    CHECK(nds_frame_count(nds) == 11);
    nds_reset(nds);
    CHECK(nds_advance_ns(nds, 10000000000ull, 100, error, sizeof error));
    CHECK(nds_frame_count(nds) == 14);

    /* The ring keeps the newest third of a second. */
    nds_reset(nds);
    for (int i = 0; i < 4; i++) CHECK(nds_advance_ns(nds, 250000000u, 100, error, sizeof error));
    CHECK(nds_audio_available(nds) > 15000 && nds_audio_available(nds) < 16384);
    nds_audio_clear(nds);
    CHECK(nds_audio_available(nds) == 0);
    nds_destroy(nds);

    /* Nothing runs without a cartridge. */
    nds = nds_create();
    CHECK(nds_advance_ns(nds, 100000000u, 100, error, sizeof error));
    CHECK(nds_frame_count(nds) == 0 && nds_audio_available(nds) == 0);
    nds_destroy(nds);
}

/* The built-in test program: both CPUs boot directly, the ARM9 uses a SWI, the
 * IPC registers, the display and VBlank IRQs through our BIOS. */
static void machine_checks(void) {
    Nds *nds = nds_create();
    const uint32_t *fb;
    nds_test_make_rom(rom, NULL);
    CHECK(nds_load_rom(nds, rom, sizeof rom, error, sizeof error));
    CHECK(nds->arm9.r[15] == 0x02000008u && nds->arm7.r[15] == 0x037F8008u);
    CHECK(nds_run_cycles(nds, NDS_CYCLES_PER_FRAME * 5, error, sizeof error));
    CHECK(nds9_read32(nds, 0x02100000u) == 14);   /* 100 / 7 */
    CHECK(nds9_read32(nds, 0x02100004u) == 2);    /* 100 % 7 */
    CHECK(nds9_read32(nds, 0x0210000Cu) == 5);    /* the ARM7's IPCSYNC value */
    CHECK(nds9_read32(nds, 0x02100008u) == 3);    /* VBlank IRQs handled */
    CHECK(nds9_read32(nds, 0x02100010u) == 0x77); /* reached the end */
    /* The ARM9 spins in its halt loop (a pending VBlank in IE & IF ends a halt
     * even with IME off); the ARM7 stays halted. */
    CHECK(nds->arm7.halted);
    CHECK(nds->arm9.undefined_count == 0 && nds->arm7.undefined_count == 0);
    /* Engine A (red backdrop) on top, engine B (green) below. */
    fb = nds_framebuffer(nds);
    CHECK(fb[0] == 0xFF0000u && fb[NDS_FRAME_WIDTH * NDS_FRAME_HEIGHT - 1] == 0x00FF00u);
    nds_destroy(nds);
}

/* The ARM9 divider's overflow cases (GBATEK, "DS Maths"). */
static void math_checks(void) {
    Nds *nds = nds_create();
    nds9_write16(nds, 0x04000280u, 0); /* 32/32 */
    /* DIV0 with nonzero upper denominator bits: no DIV0 flag, and the upper
     * half of the sign-expanded result inverted. */
    nds9_write32(nds, 0x04000290u, 123);
    nds9_write32(nds, 0x04000294u, 0);
    nds9_write32(nds, 0x04000298u, 0);
    nds9_write32(nds, 0x0400029Cu, 0x555);
    CHECK(nds9_read16(nds, 0x04000280u) == 0);
    CHECK(nds9_read32(nds, 0x040002A0u) == 0xFFFFFFFFu && nds9_read32(nds, 0x040002A4u) == 0);
    CHECK(nds9_read32(nds, 0x040002A8u) == 123 && nds9_read32(nds, 0x040002ACu) == 0);
    /* The same with the whole denominator zero: the flag as well. */
    nds9_write32(nds, 0x0400029Cu, 0);
    CHECK(nds9_read16(nds, 0x04000280u) == 0x4000);
    CHECK(nds9_read32(nds, 0x040002A0u) == 0xFFFFFFFFu && nds9_read32(nds, 0x040002A4u) == 0);
    /* -MAX / -1 gives -MAX, again with the upper half inverted. */
    nds9_write32(nds, 0x04000290u, 0x80000000u);
    nds9_write32(nds, 0x04000298u, 0xFFFFFFFFu);
    CHECK(nds9_read32(nds, 0x040002A0u) == 0x80000000u && nds9_read32(nds, 0x040002A4u) == 0);
    CHECK(nds9_read16(nds, 0x04000280u) == 0);
    /* 64/64 DIV0 is plain sign-expanded: a negative numerator gives +1. */
    nds9_write16(nds, 0x04000280u, 2);
    nds9_write32(nds, 0x04000290u, (uint32_t)-456);
    nds9_write32(nds, 0x04000294u, 0xFFFFFFF8u);
    nds9_write32(nds, 0x04000298u, 0);
    nds9_write32(nds, 0x0400029Cu, 0);
    CHECK(nds9_read32(nds, 0x040002A0u) == 1 && nds9_read32(nds, 0x040002A4u) == 0);
    CHECK(nds9_read32(nds, 0x040002A8u) == (uint32_t)-456 && nds9_read16(nds, 0x04000280u) == 0x4002);
    nds_destroy(nds);
}

static void input_checks(void) {
    Nds *nds = nds_create();
    /* Out-of-range buttons and touches are ignored rather than trusted. */
    nds_set_button(nds, NDS_BUTTON_X, 1);
    nds_set_button(nds, NDS_BUTTON_COUNT, 1);
    nds_set_button(nds, (NdsButton)-1, 1);
    nds_set_touch(nds, 255, 191, 1);
    nds_set_touch(nds, 256, 0, 1);
    nds_set_touch(nds, -1, -1, 0);
    nds_destroy(nds);
}

/* ---- headless runner ---------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    long length;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (length = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (uint8_t *)malloc((size_t)length);
        if (data && fread(data, 1, (size_t)length, f) != (size_t)length) {
            free(data);
            data = NULL;
        }
        *size = (size_t)length;
    }
    fclose(f);
    return data;
}

static int write_ppm(const char *path, const uint32_t *pixels) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", NDS_FRAME_WIDTH, NDS_FRAME_HEIGHT);
    for (int i = 0; i < NDS_FRAME_WIDTH * NDS_FRAME_HEIGHT; i++) {
        uint8_t rgb[3] = {(uint8_t)(pixels[i] >> 16), (uint8_t)(pixels[i] >> 8), (uint8_t)pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}

/* Scripted input: a button or a touch held from `frame` for `length` frames. */
typedef struct {
    int button; /* NdsButton, or -1 for a touch */
    int x, y;
    unsigned frame, length;
} Event;

/* A framebuffer hash (and dump, with a path) at the end of `frame`, or of the
 * run for UINT_MAX. */
typedef struct {
    char path[260];
    unsigned frame;
} Dump;

#define MAX_EVENTS 64

static const char *const button_names[NDS_BUTTON_COUNT] = {
    "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};

/* FNV-1a over the pixels: a short, stable name for a frame. */
static uint64_t frame_hash(const uint32_t *pixels) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < NDS_FRAME_WIDTH * NDS_FRAME_HEIGHT; i++) {
        h ^= pixels[i] & 0xFFFFFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

/* "NAME@F", "NAME@F+L" or, for touches, "X,Y@F+L". Holds default to 4 frames,
 * long enough for a program that polls once a frame to see the press. */
static int parse_event(const char *text, int touch, Event *event) {
    char name[16];
    const char *at = strchr(text, '@');
    size_t n;
    event->length = 4;
    if (!at || (n = (size_t)(at - text)) == 0 || n >= sizeof name) return 0;
    memcpy(name, text, n);
    name[n] = 0;
    if (touch) {
        event->button = -1;
        if (sscanf(name, "%d,%d", &event->x, &event->y) != 2) return 0;
    } else {
        event->button = NDS_BUTTON_COUNT;
        for (int i = 0; i < NDS_BUTTON_COUNT; i++)
            if (!strcmp(name, button_names[i])) event->button = i;
        if (event->button == NDS_BUTTON_COUNT) return 0;
    }
    return sscanf(at + 1, "%u+%u", &event->frame, &event->length) >= 1 && event->length > 0;
}

static int event_active(const Event *event, unsigned frame) {
    return frame >= event->frame && frame - event->frame < event->length;
}

static void apply_input(Nds *nds, const Event *events, int count, unsigned frame) {
    int touching = 0, tx = 0, ty = 0;
    for (int b = 0; b < NDS_BUTTON_COUNT; b++) {
        int down = 0;
        for (int i = 0; i < count; i++)
            if (events[i].button == b && event_active(&events[i], frame)) down = 1;
        nds_set_button(nds, (NdsButton)b, down);
    }
    for (int i = 0; i < count; i++)
        if (events[i].button < 0 && event_active(&events[i], frame)) {
            touching = 1;
            tx = events[i].x;
            ty = events[i].y;
        }
    nds_set_touch(nds, tx, ty, touching);
}

static int run(const char *path, unsigned frames, const Event *events, int event_count,
               const Dump *dumps, int dump_count, int bench) {
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    Nds *nds;
    int ok = 1;
    clock_t started;
    if (!data) {
        printf("can't read %s\n", path);
        return 1;
    }
    nds = nds_create();
    if (!nds_load_rom(nds, data, size, error, sizeof error)) {
        printf("%s: %s\n", path, error);
        free(data);
        nds_destroy(nds);
        return 1;
    }
    free(data);
    printf("%s [%s]\n", nds_title(nds), nds_game_code(nds));
    started = clock();
    for (unsigned f = 0; f < frames && ok; f++) {
        apply_input(nds, events, event_count, f);
        ok = nds_run_cycles(nds, NDS_CYCLES_PER_FRAME, error, sizeof error);
        for (int i = 0; i < dump_count; i++) {
            if (dumps[i].frame != f && !(dumps[i].frame == UINT_MAX && f + 1 == frames)) continue;
            printf("frame %u hash %016llx\n", f, (unsigned long long)frame_hash(nds_framebuffer(nds)));
            if (dumps[i].path[0] && !write_ppm(dumps[i].path, nds_framebuffer(nds))) {
                printf("can't write %s\n", dumps[i].path);
                ok = 0;
            }
        }
    }
    if (bench) {
        double host = (double)(clock() - started) / CLOCKS_PER_SEC;
        double emulated = (double)frames * NDS_CYCLES_PER_FRAME / NDS_ARM7_HZ;
        printf("%.2f s emulated in %.2f s: %.0f%% of real time\n", emulated, host,
               host > 0 ? 100.0 * emulated / host : 0.0);
    }
    printf("%u frames%s%s\n", nds_frame_count(nds), ok ? "" : ", faulted: ", ok ? "" : error);
    printf("hash %016llx\n", (unsigned long long)frame_hash(nds_framebuffer(nds)));
    if (nds->arm9.undefined_count || nds->arm7.undefined_count)
        printf("undefined instructions: ARM9 %u, ARM7 %u\n", (unsigned)nds->arm9.undefined_count,
               (unsigned)nds->arm7.undefined_count);
    nds_destroy(nds);
    return ok ? 0 : 1;
}

static int usage(void) {
    printf("usage: test_nds [ROM-DIR]\n"
           "       test_nds --run game.nds [--seconds N | --frames N] [--press KEY@F[+L]]...\n"
           "                [--touch X,Y@F[+L]]... [--dump [FILE][@F]]... [--bench]\n"
           "KEY: A B SELECT START RIGHT LEFT UP DOWN R L X Y. F counts frames from 0;\n"
           "holds last L frames (default 4). --dump prints the frame's hash and, given\n"
           "a FILE, writes it as a PPM; without @F it takes the last frame.\n");
    return 2;
}

static int run_main(int argc, char **argv) {
    static Event events[MAX_EVENTS];
    static Dump dumps[MAX_EVENTS];
    int event_count = 0, dump_count = 0, bench = 0;
    unsigned frames = 300;
    for (int i = 3; i < argc; i++) {
        const char *arg = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(arg, "--bench")) {
            bench = 1;
            continue;
        }
        if (!value) return usage();
        i++;
        if (!strcmp(arg, "--seconds")) {
            double seconds = atof(value);
            if (seconds <= 0 || seconds > 600) return usage();
            frames = (unsigned)(seconds * NDS_ARM7_HZ / NDS_CYCLES_PER_FRAME + 0.5);
        } else if (!strcmp(arg, "--frames")) {
            frames = (unsigned)strtoul(value, NULL, 10);
            if (frames == 0 || frames > 36000) return usage();
        } else if (!strcmp(arg, "--press") || !strcmp(arg, "--touch")) {
            if (event_count == MAX_EVENTS || !parse_event(value, arg[2] == 't', &events[event_count]))
                return usage();
            event_count++;
        } else if (!strcmp(arg, "--dump")) {
            const char *at = strrchr(value, '@');
            size_t n = at ? (size_t)(at - value) : strlen(value);
            if (dump_count == MAX_EVENTS || n >= sizeof dumps[0].path) return usage();
            memcpy(dumps[dump_count].path, value, n);
            dumps[dump_count].path[n] = 0;
            dumps[dump_count].frame = at ? (unsigned)strtoul(at + 1, NULL, 10) : UINT_MAX;
            dump_count++;
        } else {
            return usage();
        }
    }
    return run(argv[2], frames, events, event_count, dumps, dump_count, bench);
}

/* ---- test ROMs ------------------------------------------------------------ */

/* Each page's frame hash was checked by eye against the ROM's own verdicts
 * (every line OK) before it was recorded here. */

typedef struct {
    NdsButton button; /* pressed to reach the page */
    int repeat;
    const char *name;
    uint64_t hash;
} Page;

/* armwrestler: from the menu, START opens the entry under the cursor. On the
 * ARM pages START steps to the next page and from the last back to the menu;
 * the Thumb pages chain the same way. Its Thumb LDR/STR page has two checks
 * and its Thumb LDM/STM page none, so those are short by design. */
static const Page armwrestler_pages[] = {
    {NDS_BUTTON_START, 1, "ARM ALU 1", 0xb5e4993c51c84ca6ull},
    {NDS_BUTTON_START, 1, "ARM ALU 2", 0x305ed9be27e29697ull},
    {NDS_BUTTON_START, 1, "ARM LDR/STR 1", 0x7c995f19de41c805ull},
    {NDS_BUTTON_START, 1, "ARM LDR/STR 2", 0x287849302305857cull},
    {NDS_BUTTON_START, 1, "ARM LDM/STM", 0xbb3f8a3e47a53061ull},
    {NDS_BUTTON_START, 1, "back to the menu", 0},
    {NDS_BUTTON_DOWN, 6, "", 0},
    {NDS_BUTTON_START, 1, "ARM V5TE", 0xf84a96542ba808b9ull},
    {NDS_BUTTON_START, 1, "back to the menu", 0},
    {NDS_BUTTON_UP, 3, "", 0},
    {NDS_BUTTON_START, 1, "THUMB ALU", 0xae508123c702e192ull},
    {NDS_BUTTON_START, 1, "THUMB LDR/STR", 0xa2d6d64585f9dca3ull},
    {NDS_BUTTON_START, 1, "THUMB LDM/STM", 0x052b31de42a302bcull},
};

/* rockwrestler: A opens a submenu or runs a test, which ends on a screen with
 * just "OK" (the same for every test) or "FAIL nnn"; B goes back. Its main menu
 * entries, with how many tests each holds (INITIAL STATE only prints values). */
#define ROCKWRESTLER_OK 0xbae543c79bb6bc9eull
static const struct {
    const char *name;
    int tests;
} rockwrestler_menus[] = {{"ARMv4", 1}, {"ARMv5", 11}, {"IPC", 3}, {"DS MATH", 5}, {"MEMORY", 3}};

/* A press long enough for a program polling once a frame, then a release. */
static void press(Nds *nds, NdsButton button) {
    nds_set_button(nds, button, 1);
    nds_run_cycles(nds, NDS_CYCLES_PER_FRAME * 2, error, sizeof error);
    nds_set_button(nds, button, 0);
    nds_run_cycles(nds, NDS_CYCLES_PER_FRAME * 2, error, sizeof error);
}

/* Runs up to `frames` frames until the screen hashes to `want`. */
static int reaches(Nds *nds, uint64_t want, int frames) {
    for (int f = 0; f < frames; f++) {
        if (frame_hash(nds_framebuffer(nds)) == want) return 1;
        nds_run_cycles(nds, NDS_CYCLES_PER_FRAME, error, sizeof error);
    }
    return frame_hash(nds_framebuffer(nds)) == want;
}

static Nds *load_test_rom(const char *dir, const char *file) {
    char path[512];
    size_t size = 0;
    uint8_t *data;
    Nds *nds;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    if (!(data = read_file(path, &size))) {
        printf("skip %s (not downloaded; run tools/get-nds-test-roms.py)\n", file);
        return NULL;
    }
    nds = nds_create();
    if (!nds_load_rom(nds, data, size, error, sizeof error)) {
        printf("FAIL %s: %s\n", file, error);
        failures++;
        nds_destroy(nds);
        nds = NULL;
    }
    free(data);
    return nds;
}

static void armwrestler_checks(const char *dir) {
    Nds *nds = load_test_rom(dir, "armwrestler.nds");
    int passed = 0, pages = 0;
    if (!nds) return;
    nds_run_cycles(nds, NDS_CYCLES_PER_FRAME * 30, error, sizeof error);
    for (size_t i = 0; i < sizeof armwrestler_pages / sizeof armwrestler_pages[0]; i++) {
        const Page *page = &armwrestler_pages[i];
        for (int r = 0; r < page->repeat; r++) press(nds, page->button);
        if (!page->hash) continue;
        pages++;
        if (reaches(nds, page->hash, 60)) {
            passed++;
        } else {
            printf("FAIL armwrestler %s: frame hash %016llx (see test_nds --run)\n", page->name,
                   (unsigned long long)frame_hash(nds_framebuffer(nds)));
            failures++;
        }
    }
    CHECK(nds->arm9.undefined_count == 0);
    printf("armwrestler: %d/%d pages pass\n", passed, pages);
    nds_destroy(nds);
}

static void rockwrestler_checks(const char *dir) {
    Nds *nds = load_test_rom(dir, "rockwrestler.nds");
    int passed = 0, tests = 0;
    if (!nds) return;
    nds_run_cycles(nds, NDS_CYCLES_PER_FRAME * 30, error, sizeof error);
    for (size_t m = 0; m < sizeof rockwrestler_menus / sizeof rockwrestler_menus[0]; m++) {
        if (m) press(nds, NDS_BUTTON_DOWN);
        press(nds, NDS_BUTTON_A); /* into the submenu, cursor on its first test */
        for (int t = 0; t < rockwrestler_menus[m].tests; t++) {
            if (t) press(nds, NDS_BUTTON_DOWN);
            press(nds, NDS_BUTTON_A);
            tests++;
            if (reaches(nds, ROCKWRESTLER_OK, 300)) {
                passed++;
            } else {
                printf("FAIL rockwrestler %s test %d: frame hash %016llx (see test_nds --run)\n",
                       rockwrestler_menus[m].name, t + 1,
                       (unsigned long long)frame_hash(nds_framebuffer(nds)));
                failures++;
            }
            press(nds, NDS_BUTTON_B); /* back to the submenu */
        }
        press(nds, NDS_BUTTON_B); /* back to the main menu */
    }
    printf("rockwrestler: %d/%d tests pass\n", passed, tests);
    nds_destroy(nds);
}

static void rom_checks(const char *dir) {
    armwrestler_checks(dir);
    rockwrestler_checks(dir);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--run")) return argc > 2 ? run_main(argc, argv) : usage();
    if (argc > 2) return usage();
    crc_checks();
    header_checks();
    timing_checks();
    machine_checks();
    math_checks();
    input_checks();
    if (argc == 2) rom_checks(argv[1]);
    if (failures) {
        printf("test_nds: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_nds: ok\n");
    return 0;
}
