#include "pad.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define MAX_PADS 4
#define SCAN_INTERVAL_MS 500
#define STICK_THRESHOLD 16000
#define TRIGGER_THRESHOLD 12000

static SDL_Gamepad *pads[MAX_PADS];
static SDL_JoystickID pad_ids[MAX_PADS];
static int ready;
static Uint64 last_scan;
static char message[128];

/* Opens newly plugged controllers and drops unplugged ones. */
static void scan(void) {
    int count = 0;
    int i, j;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);

    for (i = 0; i < MAX_PADS; i++) {
        int present = 0;
        if (!pads[i]) continue;
        for (j = 0; j < count && ids; j++) present |= ids[j] == pad_ids[i];
        if (!present || !SDL_GamepadConnected(pads[i])) {
            SDL_CloseGamepad(pads[i]);
            pads[i] = NULL;
            snprintf(message, sizeof message, "Controller disconnected");
        }
    }
    for (j = 0; j < count && ids; j++) {
        int known = 0;
        for (i = 0; i < MAX_PADS; i++) known |= pads[i] && pad_ids[i] == ids[j];
        if (known) continue;
        for (i = 0; i < MAX_PADS; i++) {
            if (!pads[i]) {
                pads[i] = SDL_OpenGamepad(ids[j]);
                if (pads[i]) {
                    const char *name = SDL_GetGamepadName(pads[i]);
                    pad_ids[i] = ids[j];
                    snprintf(message, sizeof message, "Controller connected: %s", name ? name : "gamepad");
                }
                break;
            }
        }
    }
    SDL_free(ids);
}

void pad_init(void) {
    /* The window may not have SDL focus (the Windows and Mac frontends draw their own),
     * so let controller input through; the frontend decides when to listen. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    ready = SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : 0;
    last_scan = 0;
    if (ready) scan();
}

void pad_shutdown(void) {
    int i;
    if (!ready) return;
    for (i = 0; i < MAX_PADS; i++) {
        if (pads[i]) SDL_CloseGamepad(pads[i]);
        pads[i] = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    ready = 0;
}

static unsigned read_pad(SDL_Gamepad *pad, uint32_t labeled) {
    unsigned held = 0;
    int has_x = (labeled & NACRE_PAD_BIT(NACRE_PAD_NORTH)) != 0;
    int has_y = (labeled & NACRE_PAD_BIT(NACRE_PAD_WEST)) != 0;
    int has_l = (labeled & NACRE_PAD_BIT(NACRE_PAD_L1)) != 0;
    int has_r = (labeled & NACRE_PAD_BIT(NACRE_PAD_R1)) != 0;
    int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    int left_shoulder = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    int right_shoulder = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    int left_trigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_THRESHOLD;
    int right_trigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_THRESHOLD;
    /* The hotkey modifier is the left shoulder, unless the core uses it as L. */
    int modifier = has_l ? left_trigger : left_shoulder;
    int north = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
    int west = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    int start = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    int back = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);

    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > STICK_THRESHOLD) held |= PAD_BIT(GBI_RIGHT);
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -STICK_THRESHOLD) held |= PAD_BIT(GBI_LEFT);
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -STICK_THRESHOLD) held |= PAD_BIT(GBI_UP);
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > STICK_THRESHOLD) held |= PAD_BIT(GBI_DOWN);
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) || (north && !modifier && !has_x)) held |= PAD_BIT(GBI_A);
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) || (west && !has_y)) held |= PAD_BIT(GBI_B);
    if (north && !modifier && has_x) held |= PAD_BIT(GBI_X);
    if (west && has_y) held |= PAD_BIT(GBI_Y);
    if (left_shoulder && has_l) held |= PAD_BIT(GBI_L);
    if (right_shoulder && has_r) held |= PAD_BIT(GBI_R);
    if ((right_shoulder && !has_r) || right_trigger || (left_trigger && !has_l)) held |= PAD_BIT(GBI_FAST_FORWARD);
    /* With the left shoulder down, Start/Back/North are hotkeys instead of buttons. */
    if (modifier) {
        if (start) held |= PAD_BIT(GBI_SAVE_STATE);
        if (back) held |= PAD_BIT(GBI_LOAD_STATE);
        if (north) held |= PAD_BIT(GBI_NEXT_SLOT);
    } else {
        if (start) held |= PAD_BIT(GBI_START);
        if (back) held |= PAD_BIT(GBI_SELECT);
    }
    return held;
}

unsigned pad_poll(uint32_t labeled) {
    unsigned held = 0;
    int i;
    Uint64 now;
    if (!ready) return 0;
    SDL_UpdateGamepads();
    now = SDL_GetTicks();
    if (last_scan == 0 || now - last_scan >= SCAN_INTERVAL_MS) {
        last_scan = now ? now : 1;
        scan();
    }
    for (i = 0; i < MAX_PADS; i++) {
        if (pads[i]) held |= read_pad(pads[i], labeled);
    }
    return held;
}

const char *pad_take_message(void) {
    static char out[sizeof message];
    if (!message[0]) return NULL;
    memcpy(out, message, sizeof out);
    message[0] = '\0';
    return out;
}
