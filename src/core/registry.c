/* The list of built-in cores, file detection, and table validation. */

#include "nacre_core.h"

#include <stdio.h>
#include <string.h>

/* Order breaks probe ties: earlier wins. */
static const NacreCore *const CORES[] = {
    &nacre_gb_core,
    &nacre_nds_core,
    &nacre_chip8_core,
#ifdef NACRE_HAVE_PS2
    &nacre_ps2_core,
#endif
    NULL,
};

const NacreCore *const *nacre_cores(void) { return CORES; }

const NacreCore *nacre_core_by_id(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; CORES[i]; i++)
        if (!strcmp(CORES[i]->id, id)) return CORES[i];
    return NULL;
}

const NacreCore *nacre_core_detect(const uint8_t *head, size_t head_size, size_t size,
                                   const char *path) {
    const NacreCore *best = NULL;
    int best_score = 0;
    if (head_size > NACRE_PROBE_BYTES) head_size = NACRE_PROBE_BYTES;
    if (head_size > size) head_size = size;
    for (size_t i = 0; CORES[i]; i++) {
        int score = CORES[i]->probe(head, head_size, size, path);
        if (score > best_score) {
            best = CORES[i];
            best_score = score;
        }
    }
    return best;
}

#define REQUIRE(cond, what)                                                       \
    do {                                                                          \
        if (!(cond)) {                                                            \
            snprintf(error, error_capacity, "%s: %s", core->id ? core->id : "?", what); \
            return 0;                                                             \
        }                                                                         \
    } while (0)

int nacre_core_validate(const NacreCore *core, char *error, size_t error_capacity) {
    int has_pad = 0;
    if (!core) {
        snprintf(error, error_capacity, "no core");
        return 0;
    }
    REQUIRE(core->api_version == NACRE_CORE_API_VERSION, "built for another core API version");
    REQUIRE(core->id && *core->id && core->name && *core->name, "missing id or name");
    REQUIRE(core->probe && core->create && core->destroy && core->load && core->reset,
            "missing a lifecycle entry");
    REQUIRE(core->set_running && core->set_speed && core->error, "missing a run-control entry");
    REQUIRE(core->audio_rate && core->audio_read && core->audio_clear, "missing an audio entry");
    if (core->flags & NACRE_CORE_THREADED)
        REQUIRE(!core->advance, "threaded cores run themselves and have no advance");
    else
        REQUIRE(core->advance, "stepped cores need advance");
    if (core->flags & NACRE_CORE_NATIVE_VIDEO)
        REQUIRE(core->attach_surface, "native-video cores need attach_surface");
    else
        REQUIRE(core->video, "framebuffer cores need video");
    if (core->flags & NACRE_CORE_HAS_STATES)
        REQUIRE(core->state_size && core->state_save && core->state_load, "missing a state entry");
    if (core->flags & NACRE_CORE_HAS_BATTERY)
        REQUIRE(core->save_size && core->save_write && core->save_load && core->save_dirty &&
                    core->save_mark_clean,
                "missing a battery-save entry");
    for (int b = 0; b < NACRE_PAD_BUTTON_COUNT; b++) has_pad |= core->pad_labels[b] != NULL;
    if (has_pad) REQUIRE(core->set_pad, "has pad buttons but no set_pad");
    if (core->keypad_keys) REQUIRE(core->set_keypad, "has a keypad but no set_keypad");
    if (core->flags & NACRE_CORE_HAS_POINTER) REQUIRE(core->set_pointer, "has a pointer but no set_pointer");
    if (core->flags & NACRE_CORE_SAVE_SIZED_LATER)
        REQUIRE(core->flags & NACRE_CORE_HAS_BATTERY, "sizes saves later but has no battery saves");
    return 1;
}
