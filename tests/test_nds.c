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

/* ---- M2: timers, DMA, the cart bus, SPI and the RTC --------------------- */

/* Both CPUs halted with no IRQs enabled, so time passes without the test
 * program touching anything. */
static void park(Nds *nds) {
    for (int cpu = 0; cpu < 2; cpu++) nds->ie[cpu] = nds->ime[cpu] = 0;
    nds->arm9.halted = nds->arm7.halted = 1;
}

static Nds *idle_machine(const uint8_t *image, size_t size) {
    Nds *nds = nds_create();
    if (!nds_load_rom(nds, image, size, error, sizeof error)) {
        printf("FAIL idle_machine: %s\n", error);
        failures++;
    }
    park(nds);
    return nds;
}

static void advance(Nds *nds, uint32_t cycles) { nds_run_cycles(nds, cycles, error, sizeof error); }

static void timer_checks(void) {
    Nds *nds;
    nds_test_make_rom(rom, NULL);
    nds = idle_machine(rom, sizeof rom);
    /* ARM7 timer 0 from FF00h with its IRQ: the overflow comes 256 cycles on. */
    nds7_write32(nds, 0x04000100u, 0xFF00u | (0xC0u << 16));
    CHECK(nds7_read16(nds, 0x04000100u) == 0xFF00);
    advance(nds, 255);
    CHECK(nds7_read16(nds, 0x04000100u) == 0xFFFF && !(nds->irf[1] & NDS_IRQ_TIMER(0)));
    advance(nds, 1);
    CHECK((nds->irf[1] & NDS_IRQ_TIMER(0)) && nds7_read16(nds, 0x04000100u) == 0xFF00);
    CHECK(!(nds->irf[0] & NDS_IRQ_TIMER(0))); /* each CPU has its own timers */
    /* ARM9 timer 1 at F/64 runs on the same 33 MHz clock: 640 cycles, 10 ticks. */
    nds9_write16(nds, 0x04000104u, 0);
    nds9_write16(nds, 0x04000106u, 0x81);
    advance(nds, 640);
    CHECK(nds9_read16(nds, 0x04000104u) == 10);
    nds9_write16(nds, 0x04000106u, 0); /* stopping holds the count */
    advance(nds, 640);
    CHECK(nds9_read16(nds, 0x04000104u) == 10);
    /* The reload reaches the counter only when the timer starts. */
    nds9_write16(nds, 0x04000104u, 0x1234);
    CHECK(nds9_read16(nds, 0x04000104u) == 10);
    nds9_write16(nds, 0x04000106u, 0x80);
    CHECK(nds9_read16(nds, 0x04000104u) == 0x1234);
    /* Count-up: ARM7 timer 2 overflows every cycle; timer 3 counts those and
     * raises its IRQ on the second. */
    nds7_write32(nds, 0x0400010Cu, 0xFFFEu | (0xC4u << 16));
    nds7_write32(nds, 0x04000108u, 0xFFFFu | (0x80u << 16));
    advance(nds, 1);
    CHECK(!(nds->irf[1] & NDS_IRQ_TIMER(3)));
    advance(nds, 1);
    CHECK((nds->irf[1] & NDS_IRQ_TIMER(3)) && !(nds->irf[1] & NDS_IRQ_TIMER(2)));
    nds_destroy(nds);
}

#define DMA_ON 0x80000000u
#define DMA_IRQ (1u << 30)
#define DMA_WORD (1u << 26)
#define DMA_REPEAT (1u << 25)
#define DMA_SRC_FIXED (2u << 23)
#define DMA_DST_DOWN (1u << 21)
#define DMA9_MODE(m) ((uint32_t)(m) << 27)
#define DMA7_MODE(m) ((uint32_t)(m) << 28)

static void dma_checks(void) {
    Nds *nds;
    nds_test_make_rom(rom, NULL);
    nds = idle_machine(rom, sizeof rom);
    for (uint32_t i = 0; i < 8; i++) nds9_write32(nds, 0x02200000u + i * 4, 0x11110000u + i);
    /* Immediate, 32-bit, four words, with its IRQ; then the channel turns off. */
    nds9_write32(nds, 0x040000B0u, 0x02200000u);
    nds9_write32(nds, 0x040000B4u, 0x02210000u);
    nds9_write32(nds, 0x040000B8u, DMA_ON | DMA_IRQ | DMA_WORD | 4);
    CHECK(nds9_read32(nds, 0x02210000u) == 0x11110000u && nds9_read32(nds, 0x0221000Cu) == 0x11110003u);
    CHECK(nds9_read32(nds, 0x02210010u) == 0);
    CHECK(!(nds9_read32(nds, 0x040000B8u) & DMA_ON) && (nds->irf[0] & NDS_IRQ_DMA(0)));
    /* A fixed source fills, 16-bit; a decrementing destination runs down. */
    nds9_write32(nds, 0x040000BCu, 0x02200004u);
    nds9_write32(nds, 0x040000C0u, 0x02220006u);
    nds9_write32(nds, 0x040000C4u, DMA_ON | DMA_SRC_FIXED | DMA_DST_DOWN | 4);
    CHECK(nds9_read16(nds, 0x02220000u) == 0x0001 && nds9_read16(nds, 0x02220006u) == 0x0001);
    CHECK(nds9_read16(nds, 0x02220008u) == 0);
    /* ARM9 DMA goes past the DTCM (at 027C0000h after direct boot) to main RAM. */
    nds9_write32(nds, 0x027C0000u, 0xDEADBEEFu);
    nds9_write32(nds, 0x040000B0u, 0x02200000u);
    nds9_write32(nds, 0x040000B4u, 0x027C0000u);
    nds9_write32(nds, 0x040000B8u, DMA_ON | DMA_WORD | 1);
    CHECK(nds9_read32(nds, 0x027C0000u) == 0xDEADBEEFu);
    CHECK(nds->main_ram[0x3C0000] == 0x00 && nds->main_ram[0x3C0003] == 0x11);
    /* ARM7 VBlank DMA waits for line 192. */
    nds7_write32(nds, 0x0380F000u, 0xCAFEF00Du);
    nds7_write32(nds, 0x0380F100u, 0);
    nds7_write32(nds, 0x040000B0u, 0x0380F000u);
    nds7_write32(nds, 0x040000B4u, 0x0380F100u);
    nds7_write32(nds, 0x040000B8u, DMA_ON | DMA7_MODE(1) | DMA_WORD | 1);
    CHECK(nds7_read32(nds, 0x0380F100u) == 0);
    advance(nds, 192 * NDS_LINE_CYCLES - 1);
    CHECK(nds7_read32(nds, 0x0380F100u) == 0);
    advance(nds, 1);
    CHECK(nds7_read32(nds, 0x0380F100u) == 0xCAFEF00Du);
    nds_destroy(nds);
    /* ARM9 HBlank DMA with repeat: one word a line, on visible lines only. */
    nds = idle_machine(rom, sizeof rom);
    nds9_write32(nds, 0x02200000u, 0x5A5A5A5Au);
    nds9_write32(nds, 0x040000C8u, 0x02200000u);
    nds9_write32(nds, 0x040000CCu, 0x02230000u);
    nds9_write32(nds, 0x040000D0u, DMA_ON | DMA9_MODE(2) | DMA_WORD | DMA_REPEAT | DMA_SRC_FIXED | 1);
    advance(nds, 2 * NDS_LINE_CYCLES + NDS_HBLANK_CYCLE + 1);
    CHECK(nds9_read32(nds, 0x02230008u) == 0x5A5A5A5Au && nds9_read32(nds, 0x0223000Cu) == 0);
    CHECK(nds9_read32(nds, 0x040000D0u) & DMA_ON); /* still armed */
    advance(nds, 190 * NDS_LINE_CYCLES);
    CHECK(nds9_read32(nds, 0x022302FCu) == 0x5A5A5A5Au && nds9_read32(nds, 0x02230300u) == 0);
    nds_destroy(nds);
}

/* Sends a cart command (a byte and a big-endian word) with block size `bs`
 * at the default 6.7 MHz. */
static void cart_command(Nds *nds, uint8_t command, uint32_t param, uint32_t bs) {
    nds9_write32(nds, 0x040001A8u, command | (param >> 24) << 8 | ((param >> 16) & 0xFF) << 16 |
                                       ((param >> 8) & 0xFF) << 24);
    nds9_write32(nds, 0x040001ACu, param & 0xFF);
    nds9_write32(nds, 0x040001A4u, 0xA0000000u | bs << 24);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void cart_checks(void) {
    static uint8_t big[0x10000];
    Nds *nds;
    int same = 1;
    /* B7 cannot read below 8000h, so the image gets a patterned second half. */
    nds_test_make_rom(rom, NULL);
    memcpy(big, rom, sizeof rom);
    for (uint32_t i = sizeof rom; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + (i >> 8));
    nds = idle_machine(big, sizeof big);
    CHECK(nds_cart_chip_id(nds) == 0xC2u && nds9_read32(nds, 0x027FF800u) == 0xC2u);
    {
        /* A DSi-enhanced header (unit code 2) gets bit 30 in the chip ID. */
        Nds *dsi;
        uint16_t crc;
        big[0x12] = 2;
        crc = nds_crc16(0xFFFF, big, 0x15E);
        big[0x15E] = (uint8_t)crc;
        big[0x15F] = (uint8_t)(crc >> 8);
        dsi = idle_machine(big, sizeof big);
        CHECK(nds_cart_chip_id(dsi) == 0x400000C2u && nds9_read32(dsi, 0x027FF800u) == 0x400000C2u);
        nds_destroy(dsi);
        memcpy(big, rom, sizeof rom);
    }
    /* B7 at 8000h, 200h bytes: data at once, busy until the block's time is up
     * (5 cycles a byte for 8 command bytes and 200h data), then the IRQ. */
    nds9_write16(nds, 0x040001A0u, 0xC000);
    cart_command(nds, 0xB7, 0x8000, 1);
    CHECK(nds9_read32(nds, 0x040001A4u) & 0x00800000u);
    for (uint32_t i = 0; i < 0x200; i += 4)
        if (nds9_read32(nds, 0x04100010u) != le32(big + 0x8000 + i)) same = 0;
    CHECK(same);
    CHECK((nds9_read32(nds, 0x040001A4u) & 0x80800000u) == 0x80000000u);
    advance(nds, 5 * (8 + 0x200) - 1);
    CHECK(nds9_read32(nds, 0x040001A4u) & 0x80000000u);
    advance(nds, 1);
    CHECK(!(nds9_read32(nds, 0x040001A4u) & 0x80000000u) && (nds->irf[0] & NDS_IRQ_CART_DONE));
    /* The secure area folds onto 8000h; a read stays inside its 4 KiB block. */
    cart_command(nds, 0xB7, 0x1004, 7);
    CHECK(nds9_read32(nds, 0x04100010u) == le32(big + 0x8004));
    advance(nds, 100);
    cart_command(nds, 0xB7, 0x8F00, 1);
    for (int i = 0; i < 0x40; i++) nds9_read32(nds, 0x04100010u);
    CHECK(nds9_read32(nds, 0x04100010u) == le32(big + 0x8000));
    advance(nds, 4000);
    /* Past the image reads FFh; the chip mirrors at its 128 KiB. */
    cart_command(nds, 0xB7, 0x10000, 7);
    CHECK(nds9_read32(nds, 0x04100010u) == 0xFFFFFFFFu);
    advance(nds, 100);
    cart_command(nds, 0xB7, 0x28000, 7);
    CHECK(nds9_read32(nds, 0x04100010u) == le32(big + 0x8000));
    advance(nds, 100);
    cart_command(nds, 0xB8, 0, 7);
    CHECK(nds9_read32(nds, 0x04100010u) == 0xC2u);
    advance(nds, 100);
    /* Cart DMA the way libnds does it: one word per request, repeating. */
    nds9_write32(nds, 0x040000B0u, 0x04100010u);
    nds9_write32(nds, 0x040000B4u, 0x02240000u);
    nds9_write32(nds, 0x040000B8u, DMA_ON | DMA9_MODE(5) | DMA_WORD | DMA_REPEAT | DMA_SRC_FIXED | 1);
    cart_command(nds, 0xB7, 0x9000, 1);
    advance(nds, 5 * (8 + 0x200));
    same = 1;
    for (uint32_t i = 0; i < 0x200; i += 4)
        if (nds9_read32(nds, 0x02240000u + i) != le32(big + 0x9000 + i)) same = 0;
    CHECK(same && !(nds9_read32(nds, 0x040001A4u) & 0x80000000u));
    CHECK(nds9_read32(nds, 0x02240200u) == 0);
    /* EXMEMCNT bit 11 hands the slot to the ARM7. */
    nds9_write16(nds, 0x04000204u, 0x0800);
    CHECK(nds9_read32(nds, 0x040001A0u) == 0 && nds7_read16(nds, 0x040001A0u) == 0xC000);
    nds_destroy(nds);
}

/* One SPI byte from the ARM7, with SPICNT set first. */
static uint8_t spi(Nds *nds, uint16_t control, uint8_t byte) {
    nds7_write16(nds, 0x040001C0u, control);
    nds7_write8(nds, 0x040001C2u, byte);
    return nds7_read8(nds, 0x040001C2u);
}

#define SPI_HOLD(dev) ((uint16_t)(0x8800u | (dev) << 8))
#define SPI_LAST(dev) ((uint16_t)(0x8000u | (dev) << 8))

static uint32_t touch_read(Nds *nds, uint8_t control) {
    uint32_t hi, lo;
    spi(nds, SPI_HOLD(2), control);
    hi = spi(nds, SPI_HOLD(2), 0);
    lo = spi(nds, SPI_LAST(2), 0);
    return (hi << 8 | lo) >> 3;
}

static void flash_command(Nds *nds, uint8_t command, uint32_t address) {
    spi(nds, SPI_HOLD(1), command);
    spi(nds, SPI_HOLD(1), (uint8_t)(address >> 16));
    spi(nds, SPI_HOLD(1), (uint8_t)(address >> 8));
    spi(nds, SPI_HOLD(1), (uint8_t)address);
}

static void spi_checks(void) {
    Nds *nds;
    const uint8_t *fw;
    uint8_t got[5];
    nds_test_make_rom(rom, NULL);
    nds = idle_machine(rom, sizeof rom);
    fw = nds->firmware;
    /* The generated image's checksums hold, and boot copied the settings. */
    CHECK(nds_crc16(0, fw + 0x2C, 0x138) == (fw[0x2A] | fw[0x2B] << 8));
    CHECK(nds_crc16(0xFFFF, fw + 0x3FF00, 0x70) == (fw[0x3FF72] | fw[0x3FF73] << 8));
    CHECK(nds9_read16(nds, 0x027FFC80u) == 5 && nds9_read16(nds, 0x027FFC86u) == 'N');
    CHECK(nds9_read32(nds, 0x027FF868u) == 0x3FE00u);
    /* READ: a command, three address bytes, then data until deselect. */
    flash_command(nds, 0x03, 0x000008);
    for (int i = 0; i < 4; i++) got[i] = spi(nds, i < 3 ? SPI_HOLD(1) : SPI_LAST(1), 0);
    CHECK(!memcmp(got, "MAC\x68", 4));
    /* RDID, as a fresh command after the deselect. */
    spi(nds, SPI_HOLD(1), 0x9F);
    got[0] = spi(nds, SPI_HOLD(1), 0);
    got[1] = spi(nds, SPI_HOLD(1), 0);
    got[2] = spi(nds, SPI_LAST(1), 0);
    CHECK(got[0] == 0x20 && got[1] == 0x40 && got[2] == 0x12);
    /* WREN sets WEL; a page write needs it and clears it. */
    spi(nds, SPI_LAST(1), 0x06);
    spi(nds, SPI_HOLD(1), 0x05);
    CHECK(spi(nds, SPI_LAST(1), 0) == 0x02);
    flash_command(nds, 0x0A, 0x03FA10);
    spi(nds, SPI_LAST(1), 0xAB);
    CHECK(fw[0x3FA10] == 0xAB && fw[0x3FA11] == 0xFF);
    spi(nds, SPI_HOLD(1), 0x05);
    CHECK(spi(nds, SPI_LAST(1), 0) == 0x00);
    flash_command(nds, 0x0A, 0x03FA11);
    spi(nds, SPI_LAST(1), 0x00);
    CHECK(fw[0x3FA11] == 0xFF); /* without WEL: ignored */
    /* Touchscreen: with this calibration a 12-bit X or Y is 16 times the pixel;
     * released, X reads 0 and Y FFFh. */
    nds_set_touch(nds, 100, 50, 1);
    CHECK(touch_read(nds, 0xD0) == 1600 && touch_read(nds, 0x90) == 800);
    CHECK((touch_read(nds, 0xD8) >> 4) == 1600 >> 4); /* 8-bit mode */
    nds_set_touch(nds, -1, -1, 0);
    CHECK(touch_read(nds, 0xD0) == 0 && touch_read(nds, 0x90) == 0xFFF);
    /* A new control byte can ride along with the last reply byte. */
    nds_set_touch(nds, 10, 20, 1);
    spi(nds, SPI_HOLD(2), 0xD0);
    got[0] = spi(nds, SPI_HOLD(2), 0);
    got[1] = spi(nds, SPI_HOLD(2), 0x90);
    got[2] = spi(nds, SPI_HOLD(2), 0);
    got[3] = spi(nds, SPI_LAST(2), 0);
    CHECK(((got[0] << 8 | got[1]) >> 3) == 160 && ((got[2] << 8 | got[3]) >> 3) == 320);
    /* Power management: read register 0, write it, read it back. */
    spi(nds, SPI_HOLD(0), 0x80);
    CHECK(spi(nds, SPI_LAST(0), 0) == 0x0D);
    spi(nds, SPI_HOLD(0), 0x00);
    spi(nds, SPI_LAST(0), 0x05);
    spi(nds, SPI_HOLD(0), 0x84); /* register 4 mirrors 0 on the original DS */
    CHECK(spi(nds, SPI_LAST(0), 0) == 0x05);
    /* The SPI IRQ, when asked for. */
    CHECK(!(nds->irf[1] & NDS_IRQ_SPI));
    spi(nds, (uint16_t)(SPI_LAST(0) | 0x4000), 0x81);
    CHECK(nds->irf[1] & NDS_IRQ_SPI);
    nds_destroy(nds);
}

/* RTC bit-banging as GBATEK describes it: LSB first, data set with the clock
 * low and taken as it rises. Pins: 0 data, 1 clock, 2 select, 4-6 directions. */
static void rtc_pins(Nds *nds, unsigned pins) { nds7_write8(nds, 0x04000138u, (uint8_t)pins); }

static void rtc_send(Nds *nds, uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        unsigned bit = (byte >> i) & 1;
        rtc_pins(nds, 0x74 | bit);
        rtc_pins(nds, 0x76 | bit);
    }
}

static uint8_t rtc_receive(Nds *nds) {
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        rtc_pins(nds, 0x64);
        rtc_pins(nds, 0x66);
        byte |= (uint8_t)((nds7_read8(nds, 0x04000138u) & 1) << i);
    }
    return byte;
}

/* `command` as sent; MSB-first commands (high nibble 6) carry the read bit in bit 0. */
static void rtc_command(Nds *nds, uint8_t command, uint8_t *data, int count) {
    int reading = (command & 0x0F) == 0x06 ? command >> 7 : command & 1;
    rtc_pins(nds, 0x72);
    rtc_pins(nds, 0x76);
    rtc_send(nds, command);
    for (int i = 0; i < count; i++) {
        if (reading) data[i] = rtc_receive(nds);
        else rtc_send(nds, data[i]);
    }
    rtc_pins(nds, 0x72);
}

static void rtc_checks(void) {
    static const uint8_t expect[7] = {0x26, 0x11, 0x27, 5, 0x52, 0x34, 0x56}; /* a Friday, PM */
    uint8_t leap_day[7] = {0x28, 0x02, 0x29, 0, 0x23, 0x59, 0x59};
    Nds *nds;
    uint8_t dt[7], stat = 0;
    nds_test_make_rom(rom, NULL);
    nds = idle_machine(rom, sizeof rom);
    rtc_command(nds, 0x86, &stat, 1);
    CHECK(stat == 0x02); /* 24-hour mode */
    rtc_command(nds, 0xA6, dt, 7); /* unset, the clock starts at 2000-01-01, a Saturday */
    CHECK(dt[0] == 0x00 && dt[1] == 0x01 && dt[2] == 0x01 && dt[3] == 6 && dt[4] == 0 && dt[6] == 0);
    nds_set_wall_clock(nds, 1795782896); /* 2026-11-27 12:34:56 */
    rtc_command(nds, 0xA6, dt, 7);
    CHECK(!memcmp(dt, expect, 7));
    /* Time runs, and survives a reset. */
    advance(nds, NDS_ARM7_HZ);
    rtc_command(nds, 0xE6, dt, 3);
    CHECK(dt[0] == 0x52 && dt[1] == 0x34 && dt[2] == 0x57);
    nds_reset(nds);
    park(nds);
    rtc_command(nds, 0xE6, dt, 3);
    CHECK(dt[2] == 0x57);
    /* Setting the time keeps the date; a command sent MSB first works too. */
    dt[0] = 0x08;
    dt[1] = 0x00;
    dt[2] = 0x00;
    rtc_command(nds, 0x66, dt, 3);
    rtc_command(nds, 0x65, dt, 7); /* A6h bit-reversed */
    CHECK(dt[0] == 0x26 && dt[2] == 0x27 && dt[4] == 0x08 && dt[5] == 0x00 && dt[6] == 0x00);
    /* A whole date, 2028-02-29 23:59:59, and the second after it. */
    rtc_command(nds, 0x26, leap_day, 7);
    advance(nds, NDS_ARM7_HZ);
    rtc_command(nds, 0xA6, dt, 7);
    CHECK(dt[0] == 0x28 && dt[1] == 0x03 && dt[2] == 0x01 && dt[4] == 0x00 && dt[6] == 0x00);
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
    timer_checks();
    dma_checks();
    cart_checks();
    spi_checks();
    rtc_checks();
    if (argc == 2) rom_checks(argv[1]);
    if (failures) {
        printf("test_nds: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_nds: ok\n");
    return 0;
}
