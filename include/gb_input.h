#ifndef NACRE_GB_INPUT_H
#define NACRE_GB_INPUT_H

/* Keyboard bindings for the Game Boy frontends. The table is platform-neutral:
 * a "key" is whatever integer the frontend uses to identify a physical key (a
 * Win32 scan code, a macOS virtual key code, an SDL scancode); -1 means unbound.
 * Each action holds two keys, so the defaults can offer arrows and WASD at once. */

#include "nacre_core.h"

#include <stddef.h>

typedef enum {
    GBI_RIGHT = 0, /* the first eight match GbButton in gb.h */
    GBI_LEFT,
    GBI_UP,
    GBI_DOWN,
    GBI_A,
    GBI_B,
    GBI_SELECT,
    GBI_START,
    GBI_FAST_FORWARD, /* hold */
    GBI_SAVE_STATE,
    GBI_LOAD_STATE,
    GBI_NEXT_SLOT,
    GBI_COUNT
} GbInput;

#define GBI_BUTTONS 8
#define GBI_KEYS_PER_ACTION 2
#define GBI_UNBOUND (-1)

typedef struct {
    int key[GBI_COUNT][GBI_KEYS_PER_ACTION];
} GbBindings;

/* Bit `1u << action` per held action, as the controller code reports them. */
#define GBI_BIT(action) (1u << (action))

/* The NACRE_PAD_* bits for the held button actions (GBI_RIGHT .. GBI_START), so the
 * same bindings drive any core: A is EAST and B is SOUTH, as on a Game Boy. */
uint32_t gbi_pad_buttons(unsigned held_actions);

/* Label for the rebinding window ("Right", "Fast-forward", ...). */
const char *gbi_name(GbInput action);

/* Copies a frontend's default table into the bindings. */
void gbi_set_defaults(GbBindings *bindings, const int defaults[GBI_COUNT][GBI_KEYS_PER_ACTION]);

/* The action a key triggers, or -1. */
int gbi_lookup(const GbBindings *bindings, int key);

/* Binds `key` to one slot of an action. A key belongs to one action only, so it is
 * taken from wherever it was bound; the previous owner is returned (-1 if the key
 * was free, or if it was already on this action). Assigning GBI_UNBOUND clears the slot. */
int gbi_assign(GbBindings *bindings, GbInput action, int slot, int key);

/* Text form for preference storage, e.g. "1;77,32;...". Returns the length written,
 * or 0 if the buffer is too small. */
size_t gbi_serialize(const GbBindings *bindings, char *out, size_t capacity);

/* Reads the text form. Anything malformed (wrong count, junk, a key bound twice)
 * returns 0 and leaves `bindings` untouched. */
int gbi_parse(GbBindings *bindings, const char *text);

#endif
