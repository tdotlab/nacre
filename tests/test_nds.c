/* The DS core through its own API: cartridge header checks, timing, audio pacing,
 * input. With --run it also loads a cartridge from disk and runs it headless:
 *
 *   test_nds --run game.nds [--seconds N] [--dump frame.ppm]
 */

#include "nds.h"
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

static int run(const char *path, double seconds, const char *dump) {
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    Nds *nds;
    int ok;
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
    ok = nds_run_cycles(nds, (uint32_t)(seconds * NDS_ARM7_HZ), error, sizeof error);
    printf("%u frames%s%s\n", nds_frame_count(nds), ok ? "" : ", faulted: ", ok ? "" : error);
    if (dump && !write_ppm(dump, nds_framebuffer(nds))) {
        printf("can't write %s\n", dump);
        ok = 0;
    }
    nds_destroy(nds);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--run")) {
        double seconds = 5;
        const char *dump = NULL;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
            else if (!strcmp(argv[i], "--dump")) dump = argv[i + 1];
        }
        if (seconds <= 0 || seconds > 120) seconds = 5;
        return run(argv[2], seconds, dump);
    }
    crc_checks();
    header_checks();
    timing_checks();
    input_checks();
    if (failures) {
        printf("test_nds: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_nds: ok\n");
    return 0;
}
