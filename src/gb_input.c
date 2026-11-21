#include "gb_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FORMAT_VERSION 2
/* Version 1 had twelve actions: the eight Game Boy buttons, then the hotkeys. X, Y,
 * L and R went in after Start. */
#define V1_ACTIONS 12
#define V1_NEW_FIRST GBI_X

static const char *const names[GBI_COUNT] = {
    "Right", "Left", "Up", "Down", "A", "B", "Select", "Start", "X", "Y", "L", "R",
    "Fast-forward", "Save state", "Load state", "Next slot",
};

uint32_t gbi_pad_buttons(unsigned held_actions) {
    static const NacrePadButton pad[GBI_BUTTONS] = {
        NACRE_PAD_RIGHT, NACRE_PAD_LEFT,  NACRE_PAD_UP,     NACRE_PAD_DOWN,
        NACRE_PAD_EAST,  NACRE_PAD_SOUTH, NACRE_PAD_SELECT, NACRE_PAD_START,
        NACRE_PAD_NORTH, NACRE_PAD_WEST,  NACRE_PAD_L1,     NACRE_PAD_R1,
    };
    uint32_t buttons = 0;
    for (int action = 0; action < GBI_BUTTONS; action++)
        if (held_actions & GBI_BIT(action)) buttons |= NACRE_PAD_BIT(pad[action]);
    return buttons;
}

uint32_t gbi_labeled_buttons(const NacreCore *core) {
    uint32_t labeled = 0;
    if (!core) return 0;
    for (int b = 0; b < NACRE_PAD_BUTTON_COUNT; b++)
        if (core->pad_labels[b]) labeled |= NACRE_PAD_BIT(b);
    return labeled;
}

const char *gbi_name(GbInput action) {
    return (action >= 0 && action < GBI_COUNT) ? names[action] : "";
}

void gbi_set_defaults(GbBindings *bindings, const int defaults[GBI_COUNT][GBI_KEYS_PER_ACTION]) {
    memcpy(bindings->key, defaults, sizeof bindings->key);
}

int gbi_lookup(const GbBindings *bindings, int key) {
    int action, slot;
    if (key == GBI_UNBOUND) return -1;
    for (action = 0; action < GBI_COUNT; action++) {
        for (slot = 0; slot < GBI_KEYS_PER_ACTION; slot++) {
            if (bindings->key[action][slot] == key) return action;
        }
    }
    return -1;
}

int gbi_assign(GbBindings *bindings, GbInput action, int slot, int key) {
    int previous = -1;
    int a, s;
    if (action < 0 || action >= GBI_COUNT || slot < 0 || slot >= GBI_KEYS_PER_ACTION) return -1;
    if (key != GBI_UNBOUND) {
        for (a = 0; a < GBI_COUNT; a++) {
            for (s = 0; s < GBI_KEYS_PER_ACTION; s++) {
                if (bindings->key[a][s] == key && !(a == (int)action && s == slot)) {
                    bindings->key[a][s] = GBI_UNBOUND;
                    if (a != (int)action) previous = a;
                }
            }
        }
    }
    bindings->key[action][slot] = key;
    return previous;
}

size_t gbi_serialize(const GbBindings *bindings, char *out, size_t capacity) {
    size_t used;
    int action;
    int n = snprintf(out, capacity, "%d", FORMAT_VERSION);
    if (n < 0 || (size_t)n >= capacity) return 0;
    used = (size_t)n;
    for (action = 0; action < GBI_COUNT; action++) {
        n = snprintf(out + used, capacity - used, ";%d,%d", bindings->key[action][0], bindings->key[action][1]);
        if (n < 0 || (size_t)n >= capacity - used) return 0;
        used += (size_t)n;
    }
    return used;
}

/* Reads one integer, -1 or larger, and advances past it; 0 on failure. */
static int read_int(const char **text, int *value) {
    char *end;
    long v;
    if (**text != '-' && (**text < '0' || **text > '9')) return 0;
    v = strtol(*text, &end, 10);
    if (end == *text || v < -1 || v > 100000) return 0;
    *value = (int)v;
    *text = end;
    return 1;
}

/* Where action `n` of a version-1 form lives now. */
static int from_v1(int n) { return n < V1_NEW_FIRST ? n : n + (GBI_BUTTONS - V1_NEW_FIRST); }

static int is_new_since_v1(int action) { return action >= V1_NEW_FIRST && action < GBI_BUTTONS; }

/* 1 if `key` is bound to an action that version 1 already had. */
static int used_by_v1(const GbBindings *b, int key) {
    for (int a = 0; a < GBI_COUNT; a++) {
        if (is_new_since_v1(a)) continue;
        for (int s = 0; s < GBI_KEYS_PER_ACTION; s++)
            if (b->key[a][s] == key) return 1;
    }
    return 0;
}

int gbi_parse(GbBindings *bindings, const char *text) {
    GbBindings parsed = *bindings;
    const char *p = text;
    int version, count, n, slot, i, j;
    if (!text || !read_int(&p, &version) || (version != FORMAT_VERSION && version != 1)) return 0;
    count = version == 1 ? V1_ACTIONS : GBI_COUNT;
    for (n = 0; n < count; n++) {
        int action = version == 1 ? from_v1(n) : n;
        for (slot = 0; slot < GBI_KEYS_PER_ACTION; slot++) {
            if (*p != (slot == 0 ? ';' : ',')) return 0;
            p++;
            if (!read_int(&p, &parsed.key[action][slot])) return 0;
        }
    }
    if (*p != '\0') return 0;
    if (version == 1) {
        /* The new actions keep their keys unless the saved form put them elsewhere. */
        for (n = V1_NEW_FIRST; n < GBI_BUTTONS; n++)
            for (slot = 0; slot < GBI_KEYS_PER_ACTION; slot++)
                if (parsed.key[n][slot] != GBI_UNBOUND && used_by_v1(&parsed, parsed.key[n][slot]))
                    parsed.key[n][slot] = GBI_UNBOUND;
    }
    /* A key bound to two places would fire two actions at once. */
    for (i = 0; i < GBI_COUNT * GBI_KEYS_PER_ACTION; i++) {
        int key = parsed.key[i / GBI_KEYS_PER_ACTION][i % GBI_KEYS_PER_ACTION];
        if (key == GBI_UNBOUND) continue;
        for (j = i + 1; j < GBI_COUNT * GBI_KEYS_PER_ACTION; j++) {
            if (parsed.key[j / GBI_KEYS_PER_ACTION][j % GBI_KEYS_PER_ACTION] == key) return 0;
        }
    }
    *bindings = parsed;
    return 1;
}
