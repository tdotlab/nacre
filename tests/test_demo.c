#include "chip8.h"
#include "scheduler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    Chip8 machine;
    Chip8Scheduler scheduler;
    char error[256];
    chip8_init(&machine, 1);
    chip8_scheduler_init(&scheduler, 600);
    assert(chip8_load_rom(&machine, "assets/roms/nacre.ch8", error, sizeof(error)));
    assert(chip8_scheduler_run_cycles(&machine, &scheduler, 100, NULL, NULL, error, sizeof(error)));
    assert(machine.key_wait_active);
    unsigned lit = 0;
    for (size_t i = 0; i < sizeof(machine.display); i++) lit += machine.display[i];
    assert(lit > 100);
    uint8_t original[sizeof(machine.display)];
    memcpy(original, machine.display, sizeof(original));
    for (int tap = 0; tap < 2; tap++) {
        chip8_set_key(&machine, 1, 1);
        assert(chip8_scheduler_run_cycles(&machine, &scheduler, 1, NULL, NULL, error, sizeof(error)));
        chip8_set_key(&machine, 1, 0);
        assert(chip8_scheduler_run_cycles(&machine, &scheduler, 20, NULL, NULL, error, sizeof(error)));
        assert(machine.key_wait_active);
        assert((memcmp(original, machine.display, sizeof(original)) == 0) == (tap == 1));
    }
    puts("welcome demo tests passed");
    return 0;
}
