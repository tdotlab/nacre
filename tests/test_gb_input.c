/* Unit tests for the keyboard binding table. */
#include "gb_input.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                       \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL gb_input: %s (line %d)\n", message, __LINE__); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static const int defaults[GBI_COUNT][GBI_KEYS_PER_ACTION] = {
    {10, 11}, {20, 21}, {30, 31}, {40, 41}, {50, 51}, {60, 61}, {70, -1}, {80, 81},
    {90, -1}, {100, -1}, {110, -1}, {120, -1},
};

int main(void) {
    GbBindings b, copy, untouched;
    char text[256];
    size_t length;

    gbi_set_defaults(&b, defaults);
    CHECK(gbi_lookup(&b, 11) == GBI_RIGHT && gbi_lookup(&b, 61) == GBI_B, "defaults found by lookup");
    CHECK(gbi_lookup(&b, 999) == -1 && gbi_lookup(&b, GBI_UNBOUND) == -1, "unknown and unbound keys map to nothing");
    CHECK(strcmp(gbi_name(GBI_FAST_FORWARD), "Fast-forward") == 0 && gbi_name((GbInput)99)[0] == 0, "names");

    /* Binding a taken key moves it and reports who lost it. */
    CHECK(gbi_assign(&b, GBI_A, 0, 90) == GBI_FAST_FORWARD, "assign reports the previous owner");
    CHECK(b.key[GBI_FAST_FORWARD][0] == GBI_UNBOUND && b.key[GBI_A][0] == 90, "the key moved");
    CHECK(gbi_lookup(&b, 90) == GBI_A, "lookup follows the move");
    CHECK(gbi_assign(&b, GBI_A, 0, 90) == -1, "re-binding the same key reports no owner");
    CHECK(gbi_assign(&b, GBI_A, 1, 90) == -1 && b.key[GBI_A][0] == GBI_UNBOUND && b.key[GBI_A][1] == 90,
          "moving a key between slots of one action keeps a single copy");
    CHECK(gbi_assign(&b, GBI_B, 0, GBI_UNBOUND) == -1 && b.key[GBI_B][0] == GBI_UNBOUND, "unbinding a slot");
    CHECK(gbi_assign(&b, GBI_COUNT, 0, 5) == -1 && gbi_assign(&b, GBI_A, 2, 5) == -1,
          "out-of-range arguments are ignored");

    /* Text round trip. */
    gbi_set_defaults(&b, defaults);
    gbi_assign(&b, GBI_START, 1, GBI_UNBOUND);
    length = gbi_serialize(&b, text, sizeof text);
    CHECK(length > 0 && length == strlen(text), "serialize");
    memset(&copy, 0, sizeof copy);
    CHECK(gbi_parse(&copy, text) && memcmp(&copy, &b, sizeof b) == 0, "parse reads back what serialize wrote");
    CHECK(gbi_serialize(&b, text, 8) == 0, "a small buffer is reported");

    /* Malformed or inconsistent text is refused and changes nothing. */
    copy = b;
    untouched = b;
    CHECK(!gbi_parse(&copy, "") && !gbi_parse(&copy, "1") && !gbi_parse(&copy, "2;1,2") && !gbi_parse(&copy, NULL),
          "empty, short, wrong-version and null text");
    length = gbi_serialize(&b, text, sizeof text);
    memcpy(text + length, ";1,2", 5);
    CHECK(!gbi_parse(&copy, text), "trailing data");
    gbi_serialize(&b, text, sizeof text);
    text[2] = 'x';
    CHECK(!gbi_parse(&copy, text), "junk characters");
    {
        GbBindings twice = b;
        twice.key[GBI_UP][0] = twice.key[GBI_DOWN][1] = 55;
        gbi_serialize(&twice, text, sizeof text);
        CHECK(!gbi_parse(&copy, text), "a key bound twice is refused");
    }
    CHECK(memcmp(&copy, &untouched, sizeof copy) == 0, "bindings survive every refused parse");

    if (!failures) printf("ok   gb_input\n");
    return failures ? 1 : 0;
}
