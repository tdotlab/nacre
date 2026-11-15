/* Game Boy core checks against public test ROMs.
 *
 *   test_gb [ROM_DIR]                       run the suite (ROMs fetched by tools/get-gb-test-roms.py)
 *   test_gb --run ROM [--seconds N] [--dump OUT.pgm] [--press BUTTON:SECONDS ...]
 *                                           run one ROM and print its serial output
 *
 * The suite skips ROMs that are not present, so a fresh checkout still passes the
 * built-in checks. A missing ROM directory is reported, not treated as success of
 * the ROM checks themselves. */

#include "gb.h"
#include "../src/gb/gb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CHECK_TILEMAP: Blargg ROMs without cart RAM only report on screen; their font
 * tiles are numbered by ASCII code, so the background map reads as text. */
typedef enum { CHECK_SERIAL, CHECK_MEMORY, CHECK_FIBONACCI, CHECK_SCREEN, CHECK_TILEMAP } CheckKind;

typedef struct {
    const char *path;
    CheckKind kind;
    unsigned seconds;
    const char *reference; /* for CHECK_SCREEN: raw 160x144 shade file */
} RomCheck;

static const RomCheck checks[] = {
    {"gb-test-roms/cpu_instrs/cpu_instrs.gb", CHECK_SERIAL, 60, NULL},
    {"gb-test-roms/instr_timing/instr_timing.gb", CHECK_SERIAL, 5, NULL},
    {"gb-test-roms/mem_timing/mem_timing.gb", CHECK_SERIAL, 10, NULL},
    {"gb-test-roms/mem_timing-2/mem_timing.gb", CHECK_MEMORY, 10, NULL},
    {"gb-test-roms/halt_bug.gb", CHECK_TILEMAP, 30, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/01-registers.gb", CHECK_MEMORY, 10, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/02-len ctr.gb", CHECK_MEMORY, 10, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/03-trigger.gb", CHECK_MEMORY, 40, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/04-sweep.gb", CHECK_MEMORY, 10, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/05-sweep details.gb", CHECK_MEMORY, 10, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/06-overflow on trigger.gb", CHECK_MEMORY, 10, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/07-len sweep period sync.gb", CHECK_MEMORY, 10, NULL},
    {"gb-test-roms/dmg_sound/rom_singles/08-len ctr during power.gb", CHECK_MEMORY, 10, NULL},
    {"dmg-acid2.gb", CHECK_SCREEN, 3, "dmg-acid2.shades"},
    {"mooneye/acceptance/timer/div_write.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/rapid_toggle.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim00.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim01.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim10.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim11.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tima_reload.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim00_div_trigger.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim01_div_trigger.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim10_div_trigger.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tim11_div_trigger.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ei_sequence.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ei_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/di_timing-GS.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/halt_ime0_ei.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/halt_ime0_nointr_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/halt_ime1_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/if_ie_registers.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/intr_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/rapid_di_ei.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/reti_intr_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/oam_dma/basic.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/oam_dma/reg_read.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/bits/mem_oam.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/bits/reg_f.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/bits/unused_hwio-GS.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/instr/daa.gb", CHECK_FIBONACCI, 10, NULL},
    /* Boot state */
    {"mooneye/acceptance/boot_regs-dmgABC.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/boot_div-dmgABCmgb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/boot_hwio-dmgABCmgb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/serial/boot_sclk_align-dmgABCmgb.gb", CHECK_FIBONACCI, 10, NULL},
    /* Instruction and interrupt timing */
    {"mooneye/acceptance/add_sp_e_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/call_cc_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/call_cc_timing2.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/call_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/call_timing2.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/div_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/halt_ime1_timing2-GS.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/interrupts/ie_push.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/jp_cc_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/jp_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ld_hl_sp_e_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/pop_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/push_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ret_cc_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ret_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/reti_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/rst_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tima_write_reloading.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/timer/tma_write_reloading.gb", CHECK_FIBONACCI, 10, NULL},
    /* OAM DMA */
    {"mooneye/acceptance/oam_dma_restart.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/oam_dma_start.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/oam_dma_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/oam_dma/sources-GS.gb", CHECK_FIBONACCI, 10, NULL},
    /* PPU timing */
    {"mooneye/acceptance/ppu/hblank_ly_scx_timing-GS.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/intr_1_2_timing-GS.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/intr_2_0_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/intr_2_mode0_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/intr_2_mode0_timing_sprites.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/intr_2_mode3_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/intr_2_oam_ok_timing.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/lcdon_timing-GS.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/lcdon_write_timing-GS.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/stat_irq_blocking.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/stat_lyc_onoff.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/acceptance/ppu/vblank_stat_intr-GS.gb", CHECK_FIBONACCI, 10, NULL},
    /* OAM corruption bug. The 7-timing_effect single cannot pass: its correct
     * output (about 19 OAM dumps) overruns the 8 KiB cart-RAM log into the
     * test's own code in WRAM. The combined ROM runs it without that log and
     * checks the same CRC, so it stands in for test 7. */
    {"gb-test-roms/oam_bug/rom_singles/1-lcd_sync.gb", CHECK_MEMORY, 20, NULL},
    {"gb-test-roms/oam_bug/rom_singles/2-causes.gb", CHECK_MEMORY, 20, NULL},
    {"gb-test-roms/oam_bug/rom_singles/3-non_causes.gb", CHECK_MEMORY, 20, NULL},
    {"gb-test-roms/oam_bug/rom_singles/4-scanline_timing.gb", CHECK_MEMORY, 20, NULL},
    {"gb-test-roms/oam_bug/rom_singles/5-timing_bug.gb", CHECK_MEMORY, 20, NULL},
    {"gb-test-roms/oam_bug/rom_singles/6-timing_no_bug.gb", CHECK_MEMORY, 20, NULL},
    {"gb-test-roms/oam_bug/rom_singles/8-instr_effect.gb", CHECK_MEMORY, 20, NULL},
    {"gb-test-roms/oam_bug/oam_bug.gb", CHECK_MEMORY, 30, NULL},
    {"mooneye/emulator-only/mbc1/bits_bank1.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/bits_bank2.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/bits_mode.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/bits_ramg.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/ram_64kb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/ram_256kb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/rom_512kb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/rom_1Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/rom_2Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/rom_4Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/rom_8Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/rom_16Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc1/multicart_rom_8Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc2/bits_ramg.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc2/bits_romb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc2/bits_unused.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc2/ram.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc2/rom_512kb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc2/rom_1Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc2/rom_2Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_512kb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_1Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_2Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_4Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_8Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_16Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_32Mb.gb", CHECK_FIBONACCI, 10, NULL},
    {"mooneye/emulator-only/mbc5/rom_64Mb.gb", CHECK_FIBONACCI, 10, NULL},
};

static uint8_t *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long size;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (uint8_t *)malloc(size ? (size_t)size : 1);
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *length = (size_t)size;
    return data;
}

static int write_pgm(const char *path, const uint8_t *shades) {
    static const uint8_t gray[4] = {255, 170, 85, 0};
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return 0;
    fprintf(f, "P5\n%d %d\n255\n", GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
    for (i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) fputc(gray[shades[i] & 3], f);
    fclose(f);
    return 1;
}

/* Runs in one-second slices until the check reports a result. 1 pass, 0 fail. */
static int run_check(Gb *gb, const RomCheck *check, const char *dir, char *detail, size_t cap) {
    char error[160];
    unsigned slice;
    unsigned slices = check->seconds * 10;
    for (slice = 0; slice < slices; slice++) {
        size_t length;
        const char *serial;
        if (!gb_run_cycles(gb, GB_CLOCK_HZ / 10, error, sizeof error)) {
            snprintf(detail, cap, "%s", error);
            return 0;
        }
        serial = gb_serial_output(gb, &length);
        if (check->kind == CHECK_SERIAL) {
            if (strstr(serial, "Passed")) return 1;
            if (strstr(serial, "Failed")) {
                snprintf(detail, cap, "%s", serial);
                return 0;
            }
        } else if (check->kind == CHECK_FIBONACCI) {
            /* Search the whole log: a ROM may run its own transfers before the result. */
            static const char pass[6] = {3, 5, 8, 13, 21, 34};
            static const char fail[6] = {0x42, 0x42, 0x42, 0x42, 0x42, 0x42};
            size_t i;
            for (i = 0; i + 6 <= length; i++) {
                if (memcmp(serial + i, pass, 6) == 0) return 1;
                if (memcmp(serial + i, fail, 6) == 0) {
                    snprintf(detail, cap, "failure signature from the ROM");
                    return 0;
                }
            }
        } else if (check->kind == CHECK_TILEMAP) {
            char text[32 * 32 + 1];
            int i;
            for (i = 0; i < 32 * 32; i++) {
                uint8_t c = gb->vram[0x1800 + i];
                text[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
            }
            text[32 * 32] = 0;
            if (strstr(text, "Passed")) return 1;
            if (strstr(text, "Failed")) {
                snprintf(detail, cap, "screen reports Failed");
                return 0;
            }
        } else if (check->kind == CHECK_MEMORY) {
            const GbCart *cart = &gb->cart;
            if (cart->ram_size >= 4 && cart->ram[1] == 0xDE && cart->ram[2] == 0xB0 && cart->ram[3] == 0x61 &&
                cart->ram[0] != 0x80) {
                if (cart->ram[0] == 0) return 1;
                snprintf(detail, cap, "result code %u: %.200s", cart->ram[0], (const char *)cart->ram + 4);
                return 0;
            }
        }
    }
    if (check->kind == CHECK_SCREEN) {
        char path[1024];
        size_t length;
        uint8_t *reference;
        size_t mismatches = 0;
        size_t i;
        snprintf(path, sizeof path, "%s/%s", dir, check->reference);
        reference = read_file(path, &length);
        if (!reference || length != GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT) {
            free(reference);
            snprintf(detail, cap, "reference image %s is missing", check->reference);
            return -1;
        }
        for (i = 0; i < length; i++) mismatches += gb_framebuffer(gb)[i] != reference[i];
        free(reference);
        if (mismatches == 0) return 1;
        snprintf(path, sizeof path, "%s/%s.actual.pgm", dir, check->path);
        write_pgm(path, gb_framebuffer(gb));
        snprintf(detail, cap, "%zu pixels differ (wrote %s)", mismatches, path);
        return 0;
    }
    snprintf(detail, cap, "no result after %u seconds; serial: %.200s", check->seconds,
             gb_serial_output(gb, NULL));
    return 0;
}

/* Built-in checks that need no ROM files. */
static int builtin_checks(void) {
    static uint8_t rom[0x8000];
    static const uint8_t logo[48] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    };
    char error[160];
    int failures = 0;
    Gb *gb = gb_create();
    uint8_t save[64];

    memcpy(rom + 0x104, logo, sizeof logo);
    memcpy(rom + 0x134, "NACRETEST", 9);
    rom[0x147] = 0x00;
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01; /* JP $0150 */
    rom[0x150] = 0x3E; rom[0x151] = 0x42;                     /* LD A,$42 */
    rom[0x152] = 0xEA; rom[0x153] = 0x00; rom[0x154] = 0xC0;  /* LD ($C000),A */
    rom[0x155] = 0xD3;                                        /* illegal */

    if (!gb || !gb_rom_looks_valid(rom, sizeof rom) || !gb_load_rom(gb, rom, sizeof rom, error, sizeof error)) {
        printf("FAIL builtin: load: %s\n", error);
        gb_destroy(gb);
        return 1;
    }
    if (strcmp(gb_title(gb), "NACRETEST") != 0) { printf("FAIL builtin: title '%s'\n", gb_title(gb)); failures++; }
    if (gb_run_cycles(gb, 1000, error, sizeof error) || !strstr(error, "illegal opcode $D3")) {
        printf("FAIL builtin: illegal opcode not reported (%s)\n", error);
        failures++;
    }
    if (gb_bus_read(gb, 0xC000) != 0x42) { printf("FAIL builtin: store\n"); failures++; }

    /* CGB-only carts are refused and leave the loaded cartridge alone. */
    rom[0x143] = 0xC0;
    if (gb_load_rom(gb, rom, sizeof rom, error, sizeof error) || strcmp(gb_title(gb), "NACRETEST") != 0) {
        printf("FAIL builtin: CGB-only cart accepted\n");
        failures++;
    }
    rom[0x143] = 0;

    /* MBC3 + RTC battery round trip. */
    rom[0x147] = 0x10; rom[0x149] = 0x03;
    if (!gb_load_rom(gb, rom, sizeof rom, error, sizeof error)) {
        printf("FAIL builtin: MBC3 load: %s\n", error);
        failures++;
    } else {
        static uint8_t big[32768 + 48];
        size_t size = gb_save_size(gb);
        gb_bus_write(gb, 0x0000, 0x0A);
        gb_bus_write(gb, 0x4000, 0x02);
        gb_bus_write(gb, 0xA123, 0x77);
        gb_bus_write(gb, 0x4000, 0x08);
        gb_bus_write(gb, 0xA000, 30); /* seconds */
        gb_set_wall_clock(gb, 1000);
        if (size != 32768 + 48 || gb_save_write(gb, big, sizeof big) != size || !gb_save_dirty(gb)) {
            printf("FAIL builtin: MBC3 save size/dirty\n");
            failures++;
        }
        gb_set_wall_clock(gb, 1000 + 95);
        gb_load_rom(gb, rom, sizeof rom, error, sizeof error);
        gb_save_load(gb, big, size);
        gb_bus_write(gb, 0x0000, 0x0A);
        gb_bus_write(gb, 0x4000, 0x02);
        gb_bus_write(gb, 0x6000, 0x00);
        gb_bus_write(gb, 0x6000, 0x01);
        if (gb_bus_read(gb, 0xA123) != 0x77) { printf("FAIL builtin: MBC3 RAM restore\n"); failures++; }
        gb_bus_write(gb, 0x4000, 0x08);
        if (gb_bus_read(gb, 0xA000) != 5) { printf("FAIL builtin: RTC seconds %u\n", gb_bus_read(gb, 0xA000)); failures++; }
        gb_bus_write(gb, 0x4000, 0x09);
        if (gb_bus_read(gb, 0xA000) != 2) { printf("FAIL builtin: RTC minutes %u\n", gb_bus_read(gb, 0xA000)); failures++; }
    }
    (void)save;

    /* Joypad: selecting the buttons row shows a held Start as bit 3 low and raises the interrupt. */
    rom[0x147] = 0x00; rom[0x149] = 0;
    gb_load_rom(gb, rom, sizeof rom, error, sizeof error);
    gb_bus_write(gb, 0xFF00, 0x10);
    gb_bus_write(gb, 0xFF0F, 0x00);
    gb_set_button(gb, GB_BUTTON_START, 1);
    if ((gb_bus_read(gb, 0xFF00) & 0x0F) != 0x07 || !(gb_bus_read(gb, 0xFF0F) & GB_IRQ_JOYPAD)) {
        printf("FAIL builtin: joypad\n");
        failures++;
    }

    /* Wall-clock pacing: one second in 60 slices is one second of cycles (give or
     * take the last instruction), speed scales it, and a long stall is capped. */
    {
        int slice;
        uint64_t start;
        rom[0x155] = 0x18; rom[0x156] = 0xFE; /* JR -2 instead of the illegal opcode */
        gb_load_rom(gb, rom, sizeof rom, error, sizeof error);
        start = gb->cycles;
        for (slice = 0; slice < 60; slice++) gb_advance_ns(gb, 16666667u, 100, error, sizeof error);
        if (gb->cycles - start < GB_CLOCK_HZ || gb->cycles - start > GB_CLOCK_HZ + 24) {
            printf("FAIL builtin: 1 s at 100%% ran %llu cycles\n", (unsigned long long)(gb->cycles - start));
            failures++;
        }
        start = gb->cycles;
        gb_advance_ns(gb, 100000000u, 200, error, sizeof error);
        if (gb->cycles - start < GB_CLOCK_HZ / 5 - 24 || gb->cycles - start > GB_CLOCK_HZ / 5 + 24) {
            printf("FAIL builtin: 100 ms at 200%% ran %llu cycles\n", (unsigned long long)(gb->cycles - start));
            failures++;
        }
        start = gb->cycles;
        gb_advance_ns(gb, 5000000000u, 100, error, sizeof error);
        if (gb->cycles - start > GB_CLOCK_HZ / 4 + 24) {
            printf("FAIL builtin: a 5 s stall was not capped\n");
            failures++;
        }
    }

    gb_destroy(gb);
    if (!failures) printf("ok   builtin checks\n");
    return failures;
}

/* --press BUTTON:SECONDS holds a button (right, left, up, down, a, b, select, start)
 * for a tenth of a second starting at that time; repeat it for more presses. */
typedef struct {
    int button;
    double at;
} Press;

static int button_from_name(const char *name, size_t len) {
    static const char *const names[GB_BUTTON_COUNT] = {"right", "left", "up", "down", "a", "b", "select", "start"};
    int b;
    for (b = 0; b < GB_BUTTON_COUNT; b++) {
        if (strlen(names[b]) == len && strncmp(names[b], name, len) == 0) return b;
    }
    return -1;
}

static int run_single(int argc, char **argv) {
    const char *rom_path = argv[2];
    const char *dump = NULL;
    double seconds = 10;
    Press presses[64];
    int press_count = 0;
    size_t length;
    uint8_t *rom;
    Gb *gb;
    char error[160];
    uint32_t step = GB_CLOCK_HZ / 100;
    uint32_t total;
    uint32_t done;
    int i;
    for (i = 3; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--seconds") == 0) seconds = atof(argv[i + 1]);
        else if (strcmp(argv[i], "--dump") == 0) dump = argv[i + 1];
        else if (strcmp(argv[i], "--press") == 0) {
            const char *colon = strchr(argv[i + 1], ':');
            int button = colon ? button_from_name(argv[i + 1], (size_t)(colon - argv[i + 1])) : -1;
            if (button < 0 || press_count == 64) {
                fprintf(stderr, "bad --press %s (want e.g. start:9.5)\n", argv[i + 1]);
                return 2;
            }
            presses[press_count].button = button;
            presses[press_count].at = atof(colon + 1);
            press_count++;
        }
    }
    rom = read_file(rom_path, &length);
    if (!rom) { fprintf(stderr, "cannot read %s\n", rom_path); return 2; }
    gb = gb_create();
    if (!gb_load_rom(gb, rom, length, error, sizeof error)) { fprintf(stderr, "%s\n", error); return 2; }
    free(rom);
    total = (uint32_t)(seconds * GB_CLOCK_HZ);
    for (done = 0; done < total; done += step) {
        double now = (double)done / GB_CLOCK_HZ;
        int b;
        for (b = 0; b < GB_BUTTON_COUNT; b++) {
            int held = 0;
            for (i = 0; i < press_count; i++) {
                if (presses[i].button == b && now >= presses[i].at && now < presses[i].at + 0.1) held = 1;
            }
            gb_set_button(gb, (GbButton)b, held);
        }
        if (!gb_run_cycles(gb, step, error, sizeof error)) {
            fprintf(stderr, "%s\n", error);
            break;
        }
    }
    printf("%s\n", gb_serial_output(gb, NULL));
    if (dump && !write_pgm(dump, gb_framebuffer(gb))) fprintf(stderr, "cannot write %s\n", dump);
    gb_destroy(gb);
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".roms-cache/gb";
    size_t i;
    int failures;
    int passed = 0;
    int skipped = 0;
    int ran = 0;

    if (argc > 2 && strcmp(argv[1], "--run") == 0) return run_single(argc, argv);

    failures = builtin_checks();
    for (i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        char path[1024];
        char detail[512] = "";
        size_t length;
        uint8_t *rom;
        Gb *gb;
        char error[160];
        int result;
        snprintf(path, sizeof path, "%s/%s", dir, checks[i].path);
        rom = read_file(path, &length);
        if (!rom) { skipped++; continue; }
        gb = gb_create();
        if (!gb_load_rom(gb, rom, length, error, sizeof error)) {
            printf("FAIL %s: %s\n", checks[i].path, error);
            failures++;
            free(rom);
            gb_destroy(gb);
            continue;
        }
        free(rom);
        ran++;
        result = run_check(gb, &checks[i], dir, detail, sizeof detail);
        if (result > 0) {
            printf("ok   %s\n", checks[i].path);
            passed++;
        } else if (result < 0) {
            printf("SKIP %s: %s\n", checks[i].path, detail);
            skipped++;
            ran--;
        } else {
            printf("FAIL %s: %s\n", checks[i].path, detail);
            failures++;
        }
        gb_destroy(gb);
    }
    printf("%d/%d ROM checks passed, %d skipped (not downloaded), %d failure(s)\n", passed, ran, skipped, failures);
    return failures ? 1 : 0;
}
