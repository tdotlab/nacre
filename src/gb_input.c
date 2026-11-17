#include "gb_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FORMAT_VERSION 1

static const char *const names[GBI_COUNT] = {
    "Right", "Left", "Up", "Down", "A", "B", "Select", "Start",
    "Fast-forward", "Save state", "Load state", "Next slot",
};

uint32_t gbi_pad_buttons(unsigned held_actions) {
    static const NacrePadButton pad[GBI_BUTTONS] = {
        NACRE_PAD_RIGHT, NACRE_PAD_LEFT, NACRE_PAD_UP, NACRE_PAD_DOWN,
        NACRE_PAD_EAST,  NACRE_PAD_SOUTH, NACRE_PAD_SELECT, NACRE_PAD_START,
    };
    uint32_t buttons = 0;
    for (int action = 0; action < GBI_BUTTONS; action++)
        if (held_actions & GBI_BIT(action)) buttons |= NACRE_PAD_BIT(pad[action]);
    return buttons;
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

int gbi_parse(GbBindings *bindings, const char *text) {
    GbBindings parsed;
    const char *p = text;
    int version, action, slot, i, j;
    if (!text || !read_int(&p, &version) || version != FORMAT_VERSION) return 0;
    for (action = 0; action < GBI_COUNT; action++) {
        for (slot = 0; slot < GBI_KEYS_PER_ACTION; slot++) {
            if (*p != (slot == 0 ? ';' : ',')) return 0;
            p++;
            if (!read_int(&p, &parsed.key[action][slot])) return 0;
        }
    }
    if (*p != '\0') return 0;
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
