/* Nacre for Windows: a native Win32 shell over the shared C CHIP-8 and Game Boy cores.
 *
 * It mirrors the Mac interface: pearl sidebar, dark display well, palettes, live
 * keycaps, native menus and file dialogs, drag-and-drop, recent files, and saved
 * preferences. GDI+ draws the custom surface; SDL3 provides only the audio device,
 * exactly as on the Mac. Only the audio callback runs off the UI thread. */

#include "branding.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <timeapi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "audio.h"
#include "chip8.h"
#include "gb.h"
#include "gb_input.h"
#include "scheduler.h"
#include "../desktop/pad.h"
}
#include <SDL3/SDL.h>

#ifndef NACRE_VERSION
#define NACRE_VERSION "0.2.0"
#endif
#define NACRE_WIDEN2(text) L##text
#define NACRE_WIDEN(text) NACRE_WIDEN2(text)
#define NACRE_VERSION_W NACRE_WIDEN(NACRE_VERSION)

using namespace Gdiplus;

namespace {

/* ---- Identifiers ------------------------------------------------------- */

enum {
    IDM_OPEN = 201,
    IDM_DEMO,
    IDM_EXIT,
    IDM_PAUSE,
    IDM_RESTART,
    IDM_SOUND,
    IDM_FULLSCREEN,
    IDM_ABOUT,
    IDM_SAVE_STATE,
    IDM_LOAD_STATE,
    IDM_CONTROLS,
    IDC_SPEED,
    IDC_PALETTE,
    IDM_SPEED_BASE = 300,   /* + index */
    IDM_PALETTE_BASE = 320, /* + index */
    IDM_SLOT_BASE = 340,    /* + slot */
    IDM_RECENT_BASE = 400   /* + index */
};
const UINT WM_NACRE_LOAD = WM_APP + 1;     /* lParam: heap std::wstring* */
const UINT WM_NACRE_SNAPSHOT = WM_APP + 2; /* run the self-check and capture */
const UINT_PTR TIMER_TICK = 1;
const UINT_PTR TIMER_SNAPSHOT = 2;
const size_t MAX_ROM = CHIP8_MEMORY_SIZE - CHIP8_PROGRAM_START;
const size_t MAX_GB_ROM = 8u << 20; /* the largest MBC5 cartridge */
const int MAX_RECENT = 10;

/* ---- Palette and metrics (matches the Mac view) ------------------------ */

const unsigned C_PAGE = 0xF5F3EE, C_SIDEBAR = 0xEBE9E2, C_SIDEBAR_EDGE = 0xDAD8D0;
const unsigned C_INK = 0x293D34, C_BUTTON = 0xE1DFD8, C_BUTTON_HOT = 0xD6D4CC;
const unsigned C_BUTTON_DOWN = 0xC9C7BE, C_ACCENT = 0x527A64;
const unsigned C_WELL = 0x18211E, C_WELL_DROP = 0x294C3D, C_WELL_SHADOW = 0xD7D8CE;
const unsigned PALETTE_INK[3] = {0xC3DCC7, 0xEBD8AF, 0xE8E9DF};
const wchar_t *const PALETTE_NAME[3] = {L"Fern", L"Amber", L"Pearl"};
const uint32_t SPEED_HZ[3] = {300, 600, 1000};
const wchar_t *const SPEED_NAME[3] = {L"Relaxed · 300 Hz", L"Original · 600 Hz",
                                      L"Quick · 1000 Hz"};
const unsigned GB_SPEED_PERCENT[3] = {50, 100, 200};
const unsigned GB_FAST_PERCENT = 400; /* while the fast-forward key is held */
const int GB_STATE_SLOTS = 4;
const wchar_t *const GB_SPEED_NAME[3] = {L"Half · 0.5×", L"Normal · 1×", L"Double · 2×"};
const float SIDEBAR_W = 222.0f;
const int CLIENT_W = 1120, CLIENT_H = 730, MIN_W = 920, MIN_H = 730;

/* ---- State ------------------------------------------------------------- */

struct Audio {
    SDL_AudioStream *stream;
    SDL_AtomicInt active;
    Chip8AudioOscillator oscillator;
};

struct App {
    HINSTANCE instance;
    HWND hwnd, open, pause, restart, sound, speed, palette_button, tooltips;
    HMENU menu, recent_menu, session_menu, speed_menu, palette_menu, slot_menu;
    HACCEL accel;
    HWND hover;

    Chip8 machine;
    Chip8Scheduler scheduler;
    Audio audio;

    /* Game Boy session; `is_gb` says which machine is current. */
    Gb *gb;
    bool is_gb;
    int gb_speed;              /* index into GB_SPEED_PERCENT */
    bool gb_keys[GB_BUTTON_COUNT], shown_gb_keys[GB_BUTTON_COUNT];
    bool gb_kb[GB_BUTTON_COUNT]; /* held on the keyboard; gb_keys is keyboard or controller */
    GbBindings keys;             /* keyboard bindings, saved in the registry */
    unsigned pad_held, pad_seen; /* controller actions held now and last tick */
    bool ff_key, ff_on;          /* fast-forward key held; fast-forward running */
    int slot;                    /* save-state slot, 0-based */
    std::wstring status;         /* short-lived footer message */
    uint64_t status_until;
    uint32_t gb_frame_seen;
    std::wstring save_path;    /* empty when the cartridge has no battery */
    std::wstring save_note;
    uint64_t save_due;         /* when a dirty save should be written (ns) */
    SDL_AudioStream *gb_stream;
    bool gb_stream_running;
    Bitmap *gb_bitmap;

    std::vector<uint8_t> rom;
    std::wstring rom_path;
    bool rom_bundled;
    std::string fault;

    bool user_paused, focus_paused, dialog_open, muted, audio_available;
    bool drop_active, fullscreen, ephemeral, in_tick;
    int palette;
    uint32_t cpu_hz;
    uint64_t previous;
    uint8_t shown[CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT];
    uint8_t shown_keys[16];
    std::vector<std::wstring> recents;

    float scale; /* device pixels per logical unit (dpi / 96) */
    HDC back_dc;
    HBITMAP back_bitmap, back_old;
    int back_w, back_h;

    LONG_PTR windowed_style;
    WINDOWPLACEMENT windowed_placement;

    std::wstring snapshot_path;
    double snapshot_seconds; /* how long a launched cartridge runs before capture */
    int snapshot_status;
    std::wstring icon_font, text_font, semibold_font, mono_font;
    std::map<int, Font *> fonts;
};

App g;

/* ---- Small utilities --------------------------------------------------- */

std::wstring widen(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length <= 1) return std::wstring();
    std::wstring out((size_t)length - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], length);
    return out;
}

std::wstring file_stem(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

std::wstring file_name(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool family_exists(const wchar_t *name) {
    FontFamily family(name);
    return family.IsAvailable();
}

/* ---- Saved preferences (the Windows counterpart of NSUserDefaults) ------ */

const wchar_t *const REG_KEY = L"Software\\tdotlab\\Nacre";

DWORD reg_dword(const wchar_t *name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size) ==
        ERROR_SUCCESS)
        return value;
    return fallback;
}

void reg_set(const wchar_t *name, DWORD type, const void *data, DWORD size) {
    if (g.ephemeral) return;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) !=
        ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, type, static_cast<const BYTE *>(data), size);
    RegCloseKey(key);
}

void reg_set_dword(const wchar_t *name, DWORD value) {
    reg_set(name, REG_DWORD, &value, sizeof(value));
}

std::wstring reg_string(const wchar_t *name) {
    wchar_t buffer[512];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_SZ, NULL, buffer, &size) ==
        ERROR_SUCCESS)
        return buffer;
    return std::wstring();
}

void reg_set_string(const wchar_t *name, const std::wstring &value) {
    reg_set(name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

/* ---- Game Boy keyboard bindings ----------------------------------------- */

/* A key is its scan code (the physical position, whatever the layout), plus 0x100
 * for the extended block: arrows, keypad Enter, right Ctrl and Alt. */
int key_code(UINT scan, bool extended) { return (int)(scan & 0xFF) | (extended ? 0x100 : 0); }

const int DEFAULT_KEYS[GBI_COUNT][GBI_KEYS_PER_ACTION] = {
    {0x14D, 0x20},  /* Right: arrow, D */
    {0x14B, 0x1E},  /* Left: arrow, A */
    {0x148, 0x11},  /* Up: arrow, W */
    {0x150, 0x1F},  /* Down: arrow, S */
    {0x2D, 0x25},   /* A: X, K */
    {0x2C, 0x24},   /* B: Z, J */
    {0x0E, 0x36},   /* Select: Backspace, right Shift */
    {0x1C, 0x11C},  /* Start: Enter, keypad Enter */
    {0x0F, -1},     /* Fast-forward: Tab */
    {0x3F, -1},     /* Save state: F5 */
    {0x41, -1},     /* Load state: F7 */
    {0x40, -1},     /* Next slot: F6 */
};

/* Keys the app itself uses (Esc, Space, M, F2, F10, F11, both Ctrl and Alt keys, the
 * Windows and menu keys) cannot be bound to the game. */
bool key_reserved(int code) {
    switch (code) {
    case 0x01: case 0x39: case 0x32: case 0x3C: case 0x44: case 0x57:
    case 0x1D: case 0x11D: case 0x38: case 0x138: case 0x15B: case 0x15C: case 0x15D:
        return true;
    default:
        return false;
    }
}

std::wstring key_name(int code) {
    if (code < 0) return L"\u2014";
    LONG lparam = ((code & 0xFF) << 16) | ((code & 0x100) ? (1 << 24) : 0);
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lparam, name, 64) > 0) return name;
    wchar_t fallback[24];
    swprintf(fallback, 24, L"Key %X", code);
    return fallback;
}

/* "Save state\tF5", or just the name when nothing is bound. */
std::wstring menu_label(const wchar_t *name, GbInput action) {
    for (int slot = 0; slot < GBI_KEYS_PER_ACTION; slot++)
        if (g.keys.key[action][slot] >= 0) return std::wstring(name) + L"\t" + key_name(g.keys.key[action][slot]);
    return name;
}

/* The first key bound to an action, for hints and menu labels. */
std::wstring action_key(GbInput action) {
    for (int slot = 0; slot < GBI_KEYS_PER_ACTION; slot++)
        if (g.keys.key[action][slot] >= 0) return key_name(g.keys.key[action][slot]);
    return L"unbound";
}

void save_bindings() {
    char text[256];
    if (!gbi_serialize(&g.keys, text, sizeof(text))) return;
    reg_set_string(L"gbBindings", widen(text));
}

void load_bindings() {
    gbi_set_defaults(&g.keys, DEFAULT_KEYS);
    if (g.ephemeral) return; /* the self-check runs on the defaults */
    std::wstring stored = reg_string(L"gbBindings");
    char text[256];
    size_t length = 0;
    for (; length < stored.size() && length + 1 < sizeof(text); length++) {
        if (stored[length] > 0x7F) return; /* not ours; keep the defaults */
        text[length] = (char)stored[length];
    }
    text[length] = 0;
    if (length) gbi_parse(&g.keys, text); /* a bad value leaves the defaults */
}

void load_recents() {
    g.recents.clear();
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Recent", RRF_RT_REG_MULTI_SZ, NULL, NULL,
                     &size) != ERROR_SUCCESS || size < sizeof(wchar_t) * 2)
        return;
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Recent", RRF_RT_REG_MULTI_SZ, NULL,
                     buffer.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t *p = buffer.data(); *p && (int)g.recents.size() < MAX_RECENT;
         p += wcslen(p) + 1)
        g.recents.push_back(p);
}

void save_recents() {
    std::vector<wchar_t> buffer;
    for (const std::wstring &path : g.recents) {
        buffer.insert(buffer.end(), path.begin(), path.end());
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0');
    reg_set(L"Recent", REG_MULTI_SZ, buffer.data(), (DWORD)(buffer.size() * sizeof(wchar_t)));
}

void note_recent(const std::wstring &path) {
    for (size_t i = 0; i < g.recents.size(); i++) {
        if (_wcsicmp(g.recents[i].c_str(), path.c_str()) == 0) {
            g.recents.erase(g.recents.begin() + (std::ptrdiff_t)i);
            break;
        }
    }
    g.recents.insert(g.recents.begin(), path);
    if ((int)g.recents.size() > MAX_RECENT) g.recents.resize(MAX_RECENT);
    save_recents();
}

/* ---- Audio ------------------------------------------------------------- */

void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total) {
    (void)total;
    Audio *audio = static_cast<Audio *>(userdata);
    int16_t samples[1024];
    int left = (additional + 1) / 2;
    while (left > 0) {
        int count = left < 1024 ? left : 1024;
        chip8_audio_render_samples(&audio->oscillator, samples, (size_t)count,
                                   SDL_GetAtomicInt(&audio->active));
        if (!SDL_PutAudioStreamData(stream, samples, count * (int)sizeof(int16_t))) return;
        left -= count;
    }
}

bool paused() {
    return g.user_paused || g.focus_paused || g.dialog_open || !g.fault.empty();
}

void sync_audio() {
    SDL_SetAtomicInt(&g.audio.active, !g.is_gb && g.audio_available && !g.muted && !paused() &&
                                          g.machine.sound_timer > 0);
    bool gb_run = g.is_gb && g.gb_stream && !g.muted && !paused();
    if (gb_run != g.gb_stream_running && g.gb_stream) {
        if (gb_run) {
            SDL_ClearAudioStream(g.gb_stream);
            gb_audio_clear(g.gb);
            SDL_ResumeAudioStreamDevice(g.gb_stream);
        } else {
            SDL_PauseAudioStreamDevice(g.gb_stream);
            SDL_ClearAudioStream(g.gb_stream);
        }
        g.gb_stream_running = gb_run;
    }
}

/* Moves the samples the Game Boy produced this tick to the device. The emulator
 * runs on the wall clock and the device on its own crystal, so the queue is kept
 * between about 40 and 150 ms: top it up with silence after an underrun and drop
 * a batch when it runs long. Fast and slow speeds resample to keep up. */
void pump_gb_audio() {
    static int16_t samples[4096 * 2];
    if (!g.gb) return;
    if (!g.gb_stream_running) {
        gb_audio_clear(g.gb);
        return;
    }
    if (g.ff_on) { /* four times faster is a squeal, not music: run silent and let the queue drain */
        gb_audio_clear(g.gb);
        return;
    }
    float ratio = GB_SPEED_PERCENT[g.gb_speed] / 100.0f;
    SDL_SetAudioStreamFrequencyRatio(g.gb_stream, ratio);
    const int bytes_per_ms = (int)(GB_AUDIO_SAMPLE_RATE / 1000u) * 4;
    int queued = SDL_GetAudioStreamQueued(g.gb_stream);
    bool drop = queued > (int)(150 * bytes_per_ms * ratio);
    if (queued < (int)(20 * bytes_per_ms * ratio)) {
        static const int16_t silence[480 * 2] = {0};
        int pad = (int)(40 * bytes_per_ms * ratio) - queued;
        while (pad > 0) {
            int chunk = pad < (int)sizeof(silence) ? pad & ~3 : (int)sizeof(silence);
            if (chunk <= 0) break;
            SDL_PutAudioStreamData(g.gb_stream, silence, chunk);
            pad -= chunk;
        }
    }
    size_t frames;
    while ((frames = gb_audio_read(g.gb, samples, 4096)) > 0) {
        if (!drop) SDL_PutAudioStreamData(g.gb_stream, samples, (int)(frames * 4));
    }
}

void timer_tick(Chip8 *chip8, void *userdata) {
    (void)chip8;
    (void)userdata;
    sync_audio();
}

void release_keys() {
    for (uint8_t key = 0; key < 16; key++) chip8_set_key(&g.machine, key, 0);
    for (int b = 0; b < GB_BUTTON_COUNT; b++) {
        g.gb_kb[b] = false;
        g.gb_keys[b] = false;
        if (g.gb) gb_set_button(g.gb, (GbButton)b, 0);
    }
    g.ff_key = false;
    g.pad_held = 0;
}

/* The console sees a button as held when the keyboard or a controller holds it. */
void apply_buttons() {
    for (int b = 0; b < GB_BUTTON_COUNT; b++) {
        bool held = g.gb_kb[b] || (!paused() && (g.pad_held & PAD_BIT(b)) != 0);
        if (held != g.gb_keys[b]) {
            g.gb_keys[b] = held;
            if (g.gb) gb_set_button(g.gb, (GbButton)b, held ? 1 : 0);
        }
    }
}

/* ---- Geometry ---------------------------------------------------------- */

struct LRect {
    float x, y, w, h;
};

float logical_w() { return (float)g.back_w / g.scale; }
float logical_h() { return (float)g.back_h / g.scale; }

LRect well_rect() {
    float w = logical_w(), h = logical_h();
    return {254.0f, 111.0f, std::max(w - 286.0f, 0.0f), std::max(h - 233.0f, 0.0f)};
}

void invalidate(const LRect &r) {
    RECT rc = {(LONG)std::floor(r.x * g.scale) - 1, (LONG)std::floor(r.y * g.scale) - 1,
               (LONG)std::ceil((r.x + r.w) * g.scale) + 1,
               (LONG)std::ceil((r.y + r.h) * g.scale) + 1};
    InvalidateRect(g.hwnd, &rc, FALSE);
}

void invalidate_all() { InvalidateRect(g.hwnd, NULL, FALSE); }
void invalidate_well() {
    LRect w = well_rect();
    w.h += 4;
    invalidate(w);
}
void invalidate_keys() { invalidate({20.0f, 438.0f, 190.0f, 165.0f}); }
void invalidate_header() { invalidate({240.0f, 0.0f, std::max(logical_w() - 240.0f, 0.0f), 100.0f}); }
void invalidate_footer() {
    invalidate({240.0f, logical_h() - 44.0f, std::max(logical_w() - 240.0f, 0.0f), 44.0f});
}

/* ---- Text -------------------------------------------------------------- */

enum Face { UI, SEMIBOLD, MONO, MONO_BOLD, SERIF, ICONS };

Font *font_for(float size, Face face) {
    int key = (int)std::lround(size * 10.0f) * 8 + (int)face;
    auto found = g.fonts.find(key);
    if (found != g.fonts.end()) return found->second;
    const wchar_t *family = g.text_font.c_str();
    INT style = FontStyleRegular;
    switch (face) {
    case SEMIBOLD: family = g.semibold_font.c_str(); break;
    case MONO: family = g.mono_font.c_str(); break;
    case MONO_BOLD: family = g.mono_font.c_str(); style = FontStyleBold; break;
    case SERIF: family = L"Georgia"; break;
    case ICONS: family = g.icon_font.c_str(); break;
    default: break;
    }
    Font *font = new Font(family, size, style, UnitPixel);
    g.fonts[key] = font;
    return font;
}

void draw_text(Graphics &gfx, const wchar_t *text, float x, float y, float w, float h, float size,
               Face face, unsigned rgb, StringAlignment align = StringAlignmentNear) {
    StringFormat format(StringFormat::GenericTypographic());
    format.SetAlignment(align);
    format.SetLineAlignment(StringAlignmentNear);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(format.GetFormatFlags() | StringFormatFlagsNoWrap);
    SolidBrush brush(nacre::color(rgb));
    gfx.DrawString(text, -1, font_for(size, face), RectF(x, y, w, h), &format, &brush);
}

/* ---- Painting the window surface --------------------------------------- */

const char *KEY_LABELS = "1234QWERASDFZXCV";
const int KEY_VALUES[16] = {1, 2, 3, 12, 4, 5, 6, 13, 7, 8, 9, 14, 10, 0, 11, 15};

/* Game Boy shades run from the palette's ink (shade 0, the lightest) down to a
 * deep tone just above the well, so a Game Boy screen reads as the same phosphor
 * as CHIP-8 pixels rather than a green LCD pasted into the window. */
unsigned gb_shade_rgb(int palette, int shade) {
    const unsigned dark = 0x1E2A25;
    unsigned ink = PALETTE_INK[palette], out = 0;
    for (int channel = 0; channel < 3; channel++) {
        int shift = channel * 8;
        int a = (int)((ink >> shift) & 0xFF), b = (int)((dark >> shift) & 0xFF);
        out |= (unsigned)(a + (b - a) * shade / 3) << shift;
    }
    return out;
}

void draw_gb_display(Graphics &gfx, const LRect &well) {
    float s = g.scale;
    float scale = std::floor(std::min((well.w * s - 48.0f * s) / GB_SCREEN_WIDTH,
                                      (well.h * s - 64.0f * s) / GB_SCREEN_HEIGHT));
    if (scale < 1.0f) scale = 1.0f;
    int w = (int)scale * GB_SCREEN_WIDTH, h = (int)scale * GB_SCREEN_HEIGHT;
    int ox = (int)std::floor((well.x + well.w / 2.0f) * s - w / 2.0f);
    int oy = (int)std::floor((well.y + well.h / 2.0f) * s - h / 2.0f);
    if (!g.gb_bitmap) g.gb_bitmap = new Bitmap(GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, PixelFormat32bppRGB);
    Bitmap *frame = g.gb_bitmap;
    const uint8_t *pixels = gb_framebuffer(g.gb);
    uint32_t colors[4];
    for (int i = 0; i < 4; i++) colors[i] = 0xFF000000u | gb_shade_rgb(g.palette, i);
    BitmapData data;
    Rect all(0, 0, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
    if (frame->LockBits(&all, ImageLockModeWrite, PixelFormat32bppRGB, &data) == Ok) {
        for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
            uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(data.Scan0) + y * data.Stride);
            for (int x = 0; x < GB_SCREEN_WIDTH; x++) row[x] = colors[pixels[y * GB_SCREEN_WIDTH + x] & 3];
        }
        frame->UnlockBits(&data);
    }
    GraphicsState state = gfx.Save();
    gfx.ResetTransform();
    gfx.SetInterpolationMode(InterpolationModeNearestNeighbor);
    gfx.SetPixelOffsetMode(PixelOffsetModeHalf);
    gfx.SetSmoothingMode(SmoothingModeNone);
    gfx.DrawImage(frame, Rect(ox, oy, w, h), 0, 0, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, UnitPixel);
    gfx.Restore(state);
    g.gb_frame_seen = gb_frame_count(g.gb);
}

void draw_display(Graphics &gfx, const LRect &well) {
    if (g.is_gb) {
        draw_gb_display(gfx, well);
        return;
    }
    /* Centered integer scaling in device pixels, so every CHIP-8 pixel is crisp. */
    float s = g.scale;
    float scale = std::floor(std::min((well.w * s - 48.0f * s) / CHIP8_DISPLAY_WIDTH,
                                      (well.h * s - 64.0f * s) / CHIP8_DISPLAY_HEIGHT));
    if (scale < 1.0f) scale = 1.0f;
    int px = (int)scale;
    int ox = (int)std::floor((well.x + well.w / 2.0f) * s - CHIP8_DISPLAY_WIDTH * scale / 2.0f);
    int oy = (int)std::floor((well.y + well.h / 2.0f) * s - CHIP8_DISPLAY_HEIGHT * scale / 2.0f);
    GraphicsState state = gfx.Save();
    gfx.ResetTransform();
    gfx.SetSmoothingMode(SmoothingModeNone);
    gfx.SetPixelOffsetMode(PixelOffsetModeNone);
    SolidBrush ink(nacre::color(PALETTE_INK[g.palette]));
    for (int y = 0; y < CHIP8_DISPLAY_HEIGHT; y++) {
        int x = 0;
        while (x < CHIP8_DISPLAY_WIDTH) {
            if (!g.machine.display[y * CHIP8_DISPLAY_WIDTH + x]) {
                x++;
                continue;
            }
            int run = x;
            while (run < CHIP8_DISPLAY_WIDTH && g.machine.display[y * CHIP8_DISPLAY_WIDTH + run]) run++;
            gfx.FillRectangle(&ink, ox + x * px, oy + y * px, (run - x) * px, px);
            x = run;
        }
    }
    gfx.Restore(state);
    memcpy(g.shown, g.machine.display, sizeof(g.shown));
}

void render(Graphics &gfx, const RECT &clip) {
    gfx.ResetTransform();
    gfx.SetClip(Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.SetPixelOffsetMode(PixelOffsetModeHalf);
    gfx.ScaleTransform(g.scale, g.scale);

    float w = logical_w(), h = logical_h();
    nacre::fill_round(gfx, 0, 0, w, h, 0, C_PAGE);
    nacre::fill_round(gfx, 0, 0, SIDEBAR_W, h, 0, C_SIDEBAR);
    nacre::fill_round(gfx, SIDEBAR_W - 1, 0, 1, h, 0, C_SIDEBAR_EDGE);

    nacre::draw_shell_mark(gfx);
    draw_text(gfx, L"nacre", 66, 18, 150, 44, 31, SERIF, C_INK);
    draw_text(gfx, L"SESSION", 26, 114, 174, 18, 10, MONO_BOLD, 0x83867D);
    draw_text(gfx, L"Drop a ROM anywhere to begin.", 26, 192, 178, 32, 11, UI, 0x747B72);
    nacre::fill_round(gfx, 26, 232, 170, 1, 0, 0xD8D8CF);
    draw_text(gfx, L"Execution speed", 26, 249, 174, 20, 12, UI, 0x4A544B);
    draw_text(gfx, L"Display", 26, 325, 174, 20, 12, UI, 0x4A544B);
    draw_text(gfx, L"KEYBOARD", 26, 414, 174, 18, 10, MONO_BOLD, 0x83867D);
    auto keycap = [&](float x, float y, float cw, const wchar_t *label, bool held, float size) {
        nacre::fill_round(gfx, x, y + 2, cw, 31, 6, held ? 0x416452 : 0xD9DAD1);
        nacre::fill_round(gfx, x, y, cw, 30, 6, held ? 0x527A64 : 0xF6F5F0);
        draw_text(gfx, label, x, y + (size < 12 ? 8 : 6), cw, 20, size, MONO, held ? 0xFFFFFF : 0x596259,
                  StringAlignmentCenter);
    };
    if (g.is_gb) {
        /* A D-pad cross with A and B beside it, then Select and Start: the Game Boy's
         * own layout, labelled with the keyboard keys that press them. */
        struct Cap { int button; float col, row, cols; const wchar_t *label; float size; };
        static const Cap caps[] = {
            {GB_BUTTON_UP, 1, 0, 1, L"↑", 15},    {GB_BUTTON_LEFT, 0, 1, 1, L"←", 15},
            {GB_BUTTON_RIGHT, 2, 1, 1, L"→", 15}, {GB_BUTTON_DOWN, 1, 2, 1, L"↓", 15},
            {GB_BUTTON_A, 3, 0, 1, L"A", 12},     {GB_BUTTON_B, 3, 1, 1, L"B", 12},
            {GB_BUTTON_SELECT, 0, 3, 2, L"SELECT", 9}, {GB_BUTTON_START, 2, 3, 2, L"START", 9},
        };
        for (const Cap &cap : caps)
            keycap(26.0f + cap.col * 44, 444.0f + cap.row * 39, cap.cols * 44 - 8, cap.label,
                   g.gb_keys[cap.button], cap.size);
        draw_text(gfx, L"Rebind keys: Session ▸ Controls", 26, 610, 190, 18, 10, UI, 0x7F847A);
        std::wstring fast = g.keys.key[GBI_FAST_FORWARD][0] < 0 && g.keys.key[GBI_FAST_FORWARD][1] < 0
                                ? std::wstring(L"Fast-forward is unbound")
                                : L"Hold " + action_key(GBI_FAST_FORWARD) + L" to fast-forward";
        draw_text(gfx, fast.c_str(), 26, 628, 190, 18, 10, UI, 0x7F847A);
        std::wstring states = L"Save " + action_key(GBI_SAVE_STATE) + L"  ·  Load " +
                              action_key(GBI_LOAD_STATE) + L"  ·  Slot " + std::to_wstring(g.slot + 1);
        draw_text(gfx, states.c_str(), 26, 646, 190, 18, 10, UI, 0x7F847A);
        draw_text(gfx, L"Space to pause  ·  M to mute", 26, 664, 190, 18, 10, UI, 0x7F847A);
        draw_text(gfx, L"GAME BOY  /  160 × 144", 26, h - 36, 180, 18, 10, MONO, 0x878C80);
    } else {
        for (int i = 0; i < 16; i++) {
            wchar_t label[2] = {(wchar_t)KEY_LABELS[i], 0};
            keycap(26.0f + (i % 4) * 44, 444.0f + (i / 4) * 39, 36, label,
                   g.machine.keys[KEY_VALUES[i]] != 0, 12);
        }
        draw_text(gfx, L"Space to pause  ·  M to mute", 26, 610, 190, 18, 10, UI, 0x7F847A);
        draw_text(gfx, L"CHIP-8  /  64 × 32", 26, h - 36, 180, 18, 10, MONO, 0x878C80);
    }

    std::wstring title = g.rom_bundled ? L"Welcome to Nacre"
                         : g.rom_path.empty() ? L"Your next little world"
                                              : file_stem(g.rom_path);
    draw_text(gfx, title.c_str(), 254, 23, std::max(w - 430.0f, 0.0f), 36, 23, SEMIBOLD, C_INK);
    std::wstring kind = g.rom_bundled ? L"PRESS A KEYPAD KEY TO SAY HELLO" : L"CHIP-8 ROM";
    if (g.is_gb) {
        std::wstring cart = widen(gb_title(g.gb));
        kind = cart.empty() ? L"GAME BOY ROM" : L"GAME BOY  ·  " + cart;
    }
    draw_text(gfx, kind.c_str(), 255, 63, std::max(w - 400.0f, 0.0f), 18, 10, MONO, 0x7B8479);
    bool is_paused = paused();
    nacre::fill_round(gfx, w - 133, 42, 6, 6, 3,
                      !g.fault.empty() ? 0xB66C52 : is_paused ? 0xB09361 : 0x608C70);
    draw_text(gfx, !g.fault.empty() ? L"STOPPED" : is_paused ? L"PAUSED" : L"RUNNING", w - 118, 37,
              90, 20, 10, MONO, 0x6C786C);

    LRect well = well_rect();
    nacre::fill_round(gfx, well.x, well.y + 3, well.w, well.h, 14, C_WELL_SHADOW);
    nacre::fill_round(gfx, well.x, well.y, well.w, well.h, 14, g.drop_active ? C_WELL_DROP : C_WELL);
    draw_display(gfx, well);
    draw_text(gfx, L"NACRE", well.x + 18, well.y + well.h - 24, 100, 14, 8, MONO, 0x64776B);
    if (is_paused || g.drop_active || !g.fault.empty()) {
        const wchar_t *badge = g.drop_active ? L"Drop to open ROM"
                               : !g.fault.empty() ? L"Execution stopped"
                                                  : L"Paused";
        float bx = well.x + well.w / 2.0f - 92.0f, by = well.y + 18.0f;
        nacre::fill_round(gfx, bx, by, 184, 30, 15, 0x35483C);
        draw_text(gfx, badge, bx, by + 6, 184, 20, 12, UI, 0xDCE8DA, StringAlignmentCenter);
    }
    if (g.ff_on) {
        float bx = well.x + well.w - 112.0f, by = well.y + 18.0f;
        nacre::fill_round(gfx, bx, by, 94, 30, 15, 0x35483C);
        draw_text(gfx, L"FAST  4×", bx, by + 6, 94, 20, 12, UI, 0xDCE8DA, StringAlignmentCenter);
    }
    std::wstring note = !g.fault.empty()     ? widen(g.fault.c_str())
                        : g.is_gb && !g.status.empty() ? g.status
                        : !g.audio_available || (g.is_gb && !g.gb_stream)
                            ? std::wstring(L"Audio unavailable")
                        : g.is_gb            ? g.save_note
                                             : std::wstring();
    if (!note.empty())
        draw_text(gfx, note.c_str(), 254, h - 34, std::max(w - 450.0f, 0.0f), 18, 10, UI, 0x7A8276);
    draw_text(gfx, g.is_gb ? L"59.7 HZ FRAMES" : L"60 HZ TIMERS", w - 146, h - 34, 120, 18, 9, MONO,
              0x87907F, StringAlignmentFar);
}

void ensure_back_buffer(int w, int h) {
    if (g.back_dc && g.back_w == w && g.back_h == h) return;
    if (g.back_dc) {
        SelectObject(g.back_dc, g.back_old);
        DeleteObject(g.back_bitmap);
        DeleteDC(g.back_dc);
    }
    HDC screen = GetDC(NULL);
    g.back_dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = std::max(w, 1);
    info.bmiHeader.biHeight = -std::max(h, 1);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    g.back_bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    g.back_old = static_cast<HBITMAP>(SelectObject(g.back_dc, g.back_bitmap));
    g.back_w = std::max(w, 1);
    g.back_h = std::max(h, 1);
    /* The whole surface must be repainted at the new size. */
    Graphics gfx(g.back_dc);
    RECT all = {0, 0, g.back_w, g.back_h};
    render(gfx, all);
}

void paint_to(HDC target, const RECT &rc) {
    ensure_back_buffer(g.back_w, g.back_h);
    {
        Graphics gfx(g.back_dc);
        render(gfx, rc);
    }
    BitBlt(target, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, g.back_dc, rc.left,
           rc.top, SRCCOPY);
}

/* ---- Owner-drawn buttons ----------------------------------------------- */

struct ButtonLook {
    int id;
    bool popup;
    unsigned backdrop;
};

const ButtonLook LOOKS[] = {
    {IDM_OPEN, false, C_SIDEBAR}, {IDM_PAUSE, false, C_PAGE},    {IDM_RESTART, false, C_PAGE},
    {IDM_SOUND, false, C_PAGE},   {IDC_SPEED, true, C_SIDEBAR}, {IDC_PALETTE, true, C_SIDEBAR},
};

wchar_t glyph_for(int id) {
    switch (id) {
    case IDM_OPEN: return 0xE8B7;
    case IDM_PAUSE: return g.user_paused ? 0xE768 : 0xE769;
    case IDM_RESTART: return 0xE72C;
    case IDM_SOUND: return g.muted ? 0xE74F : 0xE767;
    default: return 0;
    }
}

void draw_button(const DRAWITEMSTRUCT *item) {
    const ButtonLook *look = NULL;
    for (const ButtonLook &candidate : LOOKS)
        if (candidate.id == (int)item->CtlID) look = &candidate;
    if (!look) return;
    int dw = item->rcItem.right - item->rcItem.left, dh = item->rcItem.bottom - item->rcItem.top;
    HDC mem = CreateCompatibleDC(item->hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(item->hDC, dw, dh);
    HGDIOBJ old = SelectObject(mem, bitmap);
    {
        Graphics gfx(mem);
        gfx.SetSmoothingMode(SmoothingModeAntiAlias);
        gfx.SetPixelOffsetMode(PixelOffsetModeHalf);
        gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        gfx.Clear(nacre::color(look->backdrop));
        gfx.ScaleTransform(g.scale, g.scale);
        float w = (float)dw / g.scale, h = (float)dh / g.scale;
        bool disabled = (item->itemState & ODS_DISABLED) != 0;
        bool down = (item->itemState & ODS_SELECTED) != 0;
        bool hot = g.hover == item->hwndItem;
        unsigned fill = disabled ? 0xE9E8E2 : down ? C_BUTTON_DOWN : hot ? C_BUTTON_HOT : C_BUTTON;
        nacre::fill_round(gfx, 0, 0, w, h, 8, fill);
        wchar_t text[64] = {};
        GetWindowTextW(item->hwndItem, text, 64);
        unsigned ink = disabled ? 0xA3A69E : 0x2F3A32;
        if (look->popup) {
            draw_text(gfx, text, 14, (h - 16) / 2, w - 44, 18, 13, UI, ink);
            Pen chevron(nacre::color(0x5E675F), 1.6f);
            chevron.SetStartCap(LineCapRound);
            chevron.SetEndCap(LineCapRound);
            chevron.SetLineJoin(LineJoinRound);
            PointF points[3] = {{w - 25.0f, h / 2 - 2.5f}, {w - 21.0f, h / 2 + 1.5f},
                                {w - 17.0f, h / 2 - 2.5f}};
            gfx.DrawLines(&chevron, points, 3);
        } else {
            wchar_t glyph[2] = {glyph_for(look->id), 0};
            if (glyph[0] && !g.icon_font.empty())
                draw_text(gfx, glyph, 12, (h - 16) / 2, 20, 18, 13, ICONS,
                          disabled ? 0xB4B7AE : look->id == IDM_OPEN ? 0x355A43 : 0x4A544B,
                          StringAlignmentCenter);
            draw_text(gfx, text, 0, (h - 16) / 2, w, 18, 13, UI, ink, StringAlignmentCenter);
        }
        if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) {
            Pen pen(nacre::color(C_ACCENT), 2.0f);
            GraphicsPath path;
            nacre::round_rect_path(path, 1.5f, 1.5f, w - 3, h - 3, 7);
            gfx.DrawPath(&pen, &path);
        }
    }
    BitBlt(item->hDC, item->rcItem.left, item->rcItem.top, dw, dh, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

LRESULT CALLBACK button_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                             DWORD_PTR data) {
    (void)id;
    (void)data;
    switch (message) {
    case WM_MOUSEMOVE:
        if (g.hover != hwnd) {
            g.hover = hwnd;
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (g.hover == hwnd) {
            g.hover = NULL;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

HWND make_button(int id, const wchar_t *tip) {
    HWND button = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)id, g.instance, NULL);
    SetWindowSubclass(button, button_proc, 1, 0);
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = g.hwnd;
    info.uId = (UINT_PTR)button;
    info.lpszText = const_cast<wchar_t *>(tip);
    SendMessageW(g.tooltips, TTM_ADDTOOLW, 0, (LPARAM)&info);
    return button;
}

void place(HWND control, float x, float y, float w, float h) {
    SetWindowPos(control, NULL, (int)std::lround(x * g.scale), (int)std::lround(y * g.scale),
                 (int)std::lround(w * g.scale), (int)std::lround(h * g.scale),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void layout() {
    float w = logical_w(), h = logical_h();
    place(g.open, 24, 148, 174, 34);
    place(g.speed, 22, 274, 178, 30);
    place(g.palette_button, 22, 350, 178, 30);
    place(g.pause, 252, h - 88, 116, 34);
    place(g.restart, 380, h - 88, 108, 34);
    place(g.sound, w - 164, h - 88, 132, 34);
}

/* Each machine keeps its own speed choice; the speed control shows the current one. */
int speed_index() {
    if (g.is_gb) return g.gb_speed;
    for (int i = 0; i < 3; i++)
        if (SPEED_HZ[i] == g.cpu_hz) return i;
    return 1;
}

const wchar_t *const *speed_names() { return g.is_gb ? GB_SPEED_NAME : SPEED_NAME; }

void update_controls() {
    bool ready = !g.rom.empty();
    SetWindowTextW(g.open, L"Open ROM…");
    SetWindowTextW(g.restart, L"Restart");
    InvalidateRect(g.open, NULL, FALSE);
    InvalidateRect(g.restart, NULL, FALSE);
    SetWindowTextW(g.pause, g.user_paused ? L"Resume" : L"Pause");
    EnableWindow(g.pause, ready && g.fault.empty());
    EnableWindow(g.restart, ready);
    SetWindowTextW(g.sound, g.muted ? L"Sound off" : L"Sound on");
    EnableWindow(g.sound, g.audio_available);
    SetWindowTextW(g.speed, speed_names()[speed_index()]);
    InvalidateRect(g.speed, NULL, FALSE);
    SetWindowTextW(g.palette_button, PALETTE_NAME[g.palette]);
    InvalidateRect(g.pause, NULL, FALSE);
    InvalidateRect(g.sound, NULL, FALSE);
    invalidate_all();
}

/* ---- Dialogs ----------------------------------------------------------- */

void show_alert(const std::wstring &message) {
    bool was_open = g.dialog_open;
    g.dialog_open = true;
    release_keys();
    sync_audio();
    TaskDialog(g.hwnd, NULL, L"Nacre", L"This ROM couldn't run", message.c_str(), TDCBF_OK_BUTTON,
               TD_WARNING_ICON, NULL);
    g.dialog_open = was_open;
    g.previous = SDL_GetTicksNS();
    SetFocus(g.hwnd);
    sync_audio();
    update_controls();
}

bool pick_rom_path(std::wstring &out) {
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return false;
    COMDLG_FILTERSPEC filters[] = {
        {L"Game ROMs (*.ch8, *.gb, *.gbc, *.rom, *.bin)", L"*.ch8;*.gb;*.gbc;*.rom;*.bin"},
        {L"CHIP-8 ROMs (*.ch8, *.rom, *.bin)", L"*.ch8;*.rom;*.bin"},
        {L"Game Boy ROMs (*.gb, *.gbc)", L"*.gb;*.gbc"},
        {L"All files", L"*.*"}};
    dialog->SetFileTypes(4, filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(L"Open a ROM");
    dialog->SetOkButtonLabel(L"Open ROM");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    bool picked = false;
    if (SUCCEEDED(dialog->Show(g.hwnd))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = path;
                CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return picked;
}

/* ---- ROM loading ------------------------------------------------------- */

/* Reads a whole file of 1..limit bytes. Returns 0 on success, -1 if it is empty or
 * too large, or the Windows error code. */
DWORD read_bytes(const std::wstring &path, size_t limit, std::vector<uint8_t> &bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() ? GetLastError() : (DWORD)-2;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || (unsigned long long)size.QuadPart > limit) {
        CloseHandle(file);
        return (DWORD)-1;
    }
    bytes.resize((size_t)size.QuadPart);
    DWORD read = 0;
    bool ok = ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, NULL) != 0 && read == bytes.size();
    DWORD code = ok ? 0 : (GetLastError() ? GetLastError() : (DWORD)-2);
    CloseHandle(file);
    return code;
}

bool read_file(const std::wstring &path, std::vector<uint8_t> &bytes, std::string &error) {
    DWORD code = read_bytes(path, MAX_GB_ROM, bytes);
    if (code == 0) return true;
    char message[256];
    if (code == (DWORD)-1)
        snprintf(message, sizeof(message),
                 "Choose a non-empty CHIP-8 or Game Boy ROM no larger than 8 MB. Your current "
                 "session has been kept.");
    else
        snprintf(message, sizeof(message),
                 "Nacre couldn't read that file (Windows error %lu). Your current session has "
                 "been kept.",
                 (unsigned long)code);
    error = message;
    return false;
}

/* ---- Game Boy battery saves -------------------------------------------- */

/* "Tetris.gb" -> "Tetris", so saves and states sit beside the ROM, as most emulators do. */
std::wstring path_stem(const std::wstring &rom_path) {
    size_t slash = rom_path.find_last_of(L"\\/");
    size_t dot = rom_path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return rom_path;
    return rom_path.substr(0, dot);
}

std::wstring save_path_for(const std::wstring &rom_path) { return path_stem(rom_path) + L".sav"; }

/* Writes to a temporary file first, then swaps it into place, so a crash mid-write
 * never leaves a torn file. */
bool write_atomic(const std::wstring &path, const uint8_t *data, size_t size) {
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = file != INVALID_HANDLE_VALUE && size > 0;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = ok && WriteFile(file, data, (DWORD)size, &written, NULL) && written == size;
        ok = FlushFileBuffers(file) && ok;
        CloseHandle(file);
    }
    if (ok)
        ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

/* Writes the cartridge RAM when it has changed: to a temporary file first, then
 * swapped into place, so a crash mid-write never leaves a torn save. */
void write_save() {
    g.save_due = 0;
    if (!g.gb || !g.is_gb || g.save_path.empty() || g.ephemeral || !gb_save_dirty(g.gb)) return;
    gb_set_wall_clock(g.gb, (int64_t)time(NULL));
    std::vector<uint8_t> data(gb_save_size(g.gb));
    size_t size = data.empty() ? 0 : gb_save_write(g.gb, data.data(), data.size());
    if (size > 0 && write_atomic(g.save_path, data.data(), size)) {
        gb_save_mark_clean(g.gb);
        g.save_note = L"Saved to " + file_name(g.save_path);
    } else {
        g.save_note = L"Couldn't write " + file_name(g.save_path);
    }
    invalidate_footer();
}

/* ---- Game Boy save states ---------------------------------------------- */

/* A short message in the footer that clears itself after a few seconds. */
void set_status(const std::wstring &text) {
    g.status = text;
    g.status_until = SDL_GetTicksNS() + 3000000000ull;
    invalidate_footer();
}

/* "Tetris.gb" keeps its states in "Tetris.state1" .. "Tetris.state4". The built-in demo
 * and anything opened without a path has nowhere to put them. */
std::wstring state_path(int slot) {
    if (g.rom_path.empty() || g.rom_bundled) return std::wstring();
    return path_stem(g.rom_path) + L".state" + std::to_wstring(slot + 1);
}

/* "Slot 2  ·  2026-11-15 14:22", or "Slot 2  ·  empty", for the menu. */
std::wstring slot_label(int slot) {
    std::wstring text = L"Slot &" + std::to_wstring(slot + 1) + L"  \u00B7  ";
    std::wstring path = state_path(slot);
    WIN32_FILE_ATTRIBUTE_DATA info;
    FILETIME local;
    SYSTEMTIME when;
    if (!path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
        FileTimeToLocalFileTime(&info.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &when)) {
        wchar_t stamp[32];
        swprintf(stamp, 32, L"%04d-%02d-%02d %02d:%02d", when.wYear, when.wMonth, when.wDay, when.wHour,
                 when.wMinute);
        return text + stamp;
    }
    return text + L"empty";
}

void save_state() {
    if (!g.is_gb || !g.gb || g.rom.empty()) return;
    std::wstring path = state_path(g.slot);
    std::wstring slot = std::to_wstring(g.slot + 1);
    if (path.empty()) {
        set_status(L"Save states need a ROM opened from a file");
        return;
    }
    std::vector<uint8_t> blob(gb_state_size(g.gb));
    size_t size = blob.empty() ? 0 : gb_state_save(g.gb, blob.data(), blob.size());
    if (size > 0 && write_atomic(path, blob.data(), size)) set_status(L"Saved state to slot " + slot);
    else set_status(L"Couldn't save to slot " + slot);
}

void load_state() {
    if (!g.is_gb || !g.gb || g.rom.empty()) return;
    std::wstring path = state_path(g.slot);
    std::wstring slot = std::to_wstring(g.slot + 1);
    if (path.empty()) {
        set_status(L"Save states need a ROM opened from a file");
        return;
    }
    std::vector<uint8_t> blob;
    DWORD code = read_bytes(path, 1u << 24, blob);
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        set_status(L"Slot " + slot + L" is empty");
    } else if (code == 0 && gb_state_load(g.gb, blob.data(), blob.size())) {
        /* The state remembers the buttons held when it was saved; what is held now wins. */
        for (int b = 0; b < GB_BUTTON_COUNT; b++) gb_set_button(g.gb, (GbButton)b, g.gb_keys[b] ? 1 : 0);
        gb_audio_clear(g.gb);
        g.fault.clear();
        g.previous = SDL_GetTicksNS();
        g.gb_frame_seen = gb_frame_count(g.gb) - 1;
        invalidate_well();
        update_controls();
        set_status(L"Loaded state from slot " + slot);
    } else {
        set_status(L"Slot " + slot + L" doesn't fit this game");
    }
}

void set_slot(int slot) {
    if (slot < 0 || slot >= GB_STATE_SLOTS) return;
    g.slot = slot;
    reg_set_dword(L"gbSlot", (DWORD)slot);
    set_status(L"State slot " + std::to_wstring(slot + 1));
    invalidate_keys();
    invalidate_all();
}

void read_save() {
    g.save_note.clear();
    if (g.save_path.empty()) return;
    gb_set_wall_clock(g.gb, (int64_t)time(NULL));
    std::vector<uint8_t> data;
    DWORD code = read_bytes(g.save_path, 1u << 20, data);
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        g.save_note = L"Saves to " + file_name(g.save_path);
    } else if (code == 0 && gb_save_load(g.gb, data.data(), data.size())) {
        g.save_note = L"Loaded " + file_name(g.save_path);
    } else {
        /* Leave a save we can't use alone rather than overwrite it. */
        g.save_note = L"Ignored " + file_name(g.save_path) + L"; it doesn't fit this cartridge";
        g.save_path.clear();
    }
}

void start_machine() {
    if (g.is_gb) {
        write_save();
        gb_reset(g.gb);
        gb_audio_clear(g.gb);
        release_keys();
        g.fault.clear();
        g.user_paused = false;
        g.previous = SDL_GetTicksNS();
        memset(g.shown_gb_keys, 0, sizeof(g.shown_gb_keys));
        return;
    }
    char error[256];
    Chip8 candidate;
    chip8_init(&candidate, 0x4E414352u);
    if (!chip8_load_bytes(&candidate, g.rom.data(), g.rom.size(), error, sizeof(error))) return;
    g.machine = candidate;
    g.fault.clear();
    g.user_paused = false;
    chip8_scheduler_init(&g.scheduler, g.cpu_hz);
    g.previous = SDL_GetTicksNS();
    memset(g.shown_keys, 0, sizeof(g.shown_keys));
}

/* A file with a Game Boy cartridge header runs on the Game Boy core; anything
 * else must be a CHIP-8 program. A failed load keeps the current session. */
void load_candidate(const std::vector<uint8_t> &bytes, const std::wstring &path, bool bundled) {
    char error[256];
    bool gb_rom = !bundled && gb_rom_looks_valid(bytes.data(), bytes.size());
    if (gb_rom) {
        if (!g.gb) {
            show_alert(L"The Game Boy core couldn't start, so this ROM can't run.");
            return;
        }
        write_save(); /* the cartridge being replaced */
        if (!gb_load_rom(g.gb, bytes.data(), bytes.size(), error, sizeof(error))) {
            show_alert(widen(error) + L" Your current session has been kept.");
            return;
        }
    } else {
        if (bytes.size() > MAX_ROM) {
            show_alert(L"That file has no Game Boy cartridge header, and a CHIP-8 ROM can be at "
                       L"most 3,584 bytes. Your current session has been kept.");
            return;
        }
        Chip8 candidate;
        chip8_init(&candidate, 0x4E414352u);
        if (!chip8_load_bytes(&candidate, bytes.data(), bytes.size(), error, sizeof(error))) {
            show_alert(widen(error));
            return;
        }
        write_save();
    }
    g.is_gb = gb_rom;
    g.rom = bytes;
    g.rom_path = path;
    g.rom_bundled = bundled;
    g.save_path.clear();
    g.save_note.clear();
    if (gb_rom) {
        if (gb_has_battery(g.gb) && !path.empty()) {
            g.save_path = save_path_for(path);
            read_save();
        }
        g.gb_frame_seen = gb_frame_count(g.gb) - 1;
        g.fault.clear();
        g.user_paused = false;
        g.previous = SDL_GetTicksNS();
        release_keys();
        memset(g.shown_gb_keys, 0, sizeof(g.shown_gb_keys));
    } else {
        start_machine();
    }
    if (!bundled && !path.empty()) note_recent(path);
    SetFocus(g.hwnd);
    sync_audio();
    update_controls();
}

void load_file(const std::wstring &path) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!read_file(path, bytes, error)) {
        show_alert(widen(error.c_str()));
        return;
    }
    load_candidate(bytes, path, false);
}

void load_demo() {
    HRSRC resource = FindResourceW(NULL, L"NACRE_ROM", RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(NULL, resource) : NULL;
    const uint8_t *data = loaded ? static_cast<const uint8_t *>(LockResource(loaded)) : NULL;
    if (!data) {
        show_alert(L"The welcome ROM is missing from this build.");
        return;
    }
    std::vector<uint8_t> bytes(data, data + SizeofResource(NULL, resource));
    load_candidate(bytes, L"", true);
}

/* ---- Commands ---------------------------------------------------------- */

void open_rom() {
    if (g.dialog_open) return;
    g.dialog_open = true;
    release_keys();
    sync_audio();
    std::wstring path;
    bool picked = pick_rom_path(path);
    g.dialog_open = false;
    g.previous = SDL_GetTicksNS();
    SetFocus(g.hwnd);
    sync_audio();
    if (picked) load_file(path);
    update_controls();
}

void toggle_pause() {
    if (g.rom.empty() || !g.fault.empty()) return;
    g.user_paused = !g.user_paused;
    release_keys();
    sync_audio();
    update_controls();
    SetFocus(g.hwnd);
}

void restart_rom() {
    if (g.rom.empty()) return;
    start_machine();
    sync_audio();
    update_controls();
    SetFocus(g.hwnd);
}

void toggle_sound() {
    g.muted = !g.muted;
    reg_set_dword(L"muted", g.muted ? 1 : 0);
    sync_audio();
    update_controls();
    SetFocus(g.hwnd);
}

void set_palette(int index) {
    if (index < 0 || index > 2) return;
    g.palette = index;
    reg_set_dword(L"palette", (DWORD)index);
    update_controls();
    SetFocus(g.hwnd);
}

void set_speed(int index) {
    if (index < 0 || index > 2) return;
    if (g.is_gb) {
        g.gb_speed = index;
        reg_set_dword(L"gbSpeed", (DWORD)index);
        update_controls();
        SetFocus(g.hwnd);
        return;
    }
    g.cpu_hz = SPEED_HZ[index];
    chip8_scheduler_set_cpu_hz(&g.scheduler, g.cpu_hz);
    reg_set_dword(L"cpuHz", g.cpu_hz);
    update_controls();
    SetFocus(g.hwnd);
}

void toggle_fullscreen() {
    if (!g.fullscreen) {
        g.windowed_placement.length = sizeof(g.windowed_placement);
        GetWindowPlacement(g.hwnd, &g.windowed_placement);
        g.windowed_style = GetWindowLongPtrW(g.hwnd, GWL_STYLE);
        MONITORINFO info = {sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &info);
        SetMenu(g.hwnd, NULL);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, (g.windowed_style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g.hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g.fullscreen = true;
    } else {
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, g.windowed_style);
        SetMenu(g.hwnd, g.menu);
        SetWindowPlacement(g.hwnd, &g.windowed_placement);
        SetWindowPos(g.hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g.fullscreen = false;
    }
    SetFocus(g.hwnd);
}

void show_about() {
    bool was_open = g.dialog_open;
    g.dialog_open = true;
    release_keys();
    sync_audio();
    TaskDialog(g.hwnd, NULL, L"About Nacre", L"Nacre " NACRE_VERSION_W,
               L"A small CHIP-8 and Game Boy machine.\nMade by tdotlab.\n\nFree software under the GNU GPL, "
               L"version 3 or later (see LICENSE).\nSound uses SDL3, bundled under its zlib license "
               L"(see THIRD_PARTY_NOTICES.txt).",
               TDCBF_OK_BUTTON, TD_INFORMATION_ICON, NULL);
    g.dialog_open = was_open;
    g.previous = SDL_GetTicksNS();
    sync_audio();
    update_controls();
}

void choose_popup(HWND anchor, const wchar_t *const *names, int base, int selected, int count) {
    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < count; i++) AppendMenuW(menu, MF_STRING, (UINT_PTR)(base + i), names[i]);
    CheckMenuRadioItem(menu, 0, (UINT)count - 1, (UINT)selected, MF_BYPOSITION);
    RECT rc;
    GetWindowRect(anchor, &rc);
    int command = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                                 rc.left, rc.bottom + 2, 0, g.hwnd, NULL);
    DestroyMenu(menu);
    if (command >= base && command < base + count) {
        if (base == IDM_SPEED_BASE) set_speed(command - base);
        else set_palette(command - base);
    } else {
        SetFocus(g.hwnd);
    }
}

/* ---- Controls window: keyboard rebinding -------------------------------- */

namespace controls {

const int ID_SLOT_BASE = 7000; /* + action * 2 + slot */
const int ID_RESET = 7100;
const int ID_DONE = 7101;

struct Dialog {
    HWND window, hint, done;
    HWND slot[GBI_COUNT][GBI_KEYS_PER_ACTION];
    HFONT font;
    int capture; /* action * 2 + slot being rebound, or -1 */
    bool running;
    std::wstring message;
};
Dialog dlg;

void refresh() {
    for (int a = 0; a < GBI_COUNT; a++) {
        for (int k = 0; k < GBI_KEYS_PER_ACTION; k++) {
            bool capturing = dlg.capture == a * GBI_KEYS_PER_ACTION + k;
            std::wstring text = capturing ? std::wstring(L"Press a key…") : key_name(g.keys.key[a][k]);
            SetWindowTextW(dlg.slot[a][k], text.c_str());
        }
    }
    const wchar_t *hint = dlg.capture >= 0 ? L"Press the new key. Esc cancels, Del clears."
                          : dlg.message.empty() ? L"Click a key to change it. Every action takes two keys."
                                                : dlg.message.c_str();
    SetWindowTextW(dlg.hint, hint);
}

void capture_key(LPARAM lparam) {
    int code = key_code((UINT)((lparam >> 16) & 0xFF), (lparam & (1 << 24)) != 0);
    int action = dlg.capture / GBI_KEYS_PER_ACTION, slot = dlg.capture % GBI_KEYS_PER_ACTION;
    dlg.message.clear();
    if (code == 0x01) {
        dlg.capture = -1; /* Esc: leave it as it was */
    } else if (code == 0x153) { /* Del */
        gbi_assign(&g.keys, (GbInput)action, slot, GBI_UNBOUND);
        save_bindings();
        dlg.capture = -1;
    } else if (key_reserved(code)) {
        dlg.message = L"Nacre uses " + key_name(code) + L" itself. Pick another key.";
        dlg.capture = -1;
    } else {
        int previous = gbi_assign(&g.keys, (GbInput)action, slot, code);
        if (previous >= 0)
            dlg.message = key_name(code) + L" moved here from " + widen(gbi_name((GbInput)previous)) + L".";
        save_bindings();
        dlg.capture = -1;
    }
    refresh();
}

LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        int id = (int)LOWORD(wparam);
        if (HIWORD(wparam) != BN_CLICKED) break;
        if (id >= ID_SLOT_BASE && id < ID_SLOT_BASE + GBI_COUNT * GBI_KEYS_PER_ACTION) {
            dlg.capture = id - ID_SLOT_BASE;
            dlg.message.clear();
            refresh();
        } else if (id == ID_RESET) {
            gbi_set_defaults(&g.keys, DEFAULT_KEYS);
            save_bindings();
            dlg.capture = -1;
            dlg.message = L"Keys restored to the defaults.";
            refresh();
        } else if (id == ID_DONE || id == IDCANCEL) {
            dlg.running = false;
        }
        return 0;
    }
    case WM_CLOSE:
        dlg.running = false;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void show() {
    if (g.dialog_open) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls = {sizeof(cls)};
        cls.lpfnWndProc = proc;
        cls.hInstance = g.instance;
        cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
        cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        cls.hIcon = LoadIconW(g.instance, MAKEINTRESOURCEW(1));
        cls.lpszClassName = L"NacreControls";
        if (!RegisterClassExW(&cls)) return;
        registered = true;
    }
    UINT dpi = GetDpiForWindow(g.hwnd);
    auto S = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    const int CLIENT_CW = 430, CLIENT_CH = 524;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    RECT rc = {0, 0, S(CLIENT_CW), S(CLIENT_CH)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = owner.left + ((owner.right - owner.left) - w) / 2;
    int y = owner.top + ((owner.bottom - owner.top) - h) / 2;
    dlg = Dialog();
    dlg.capture = -1;
    dlg.window = CreateWindowExW(ex, L"NacreControls", L"Game Boy controls", style, x, y, w, h, g.hwnd, NULL,
                                 g.instance, NULL);
    if (!dlg.window) return;

    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
        dlg.font = CreateFontIndirectW(&metrics.lfMessageFont);
    auto child = [&](const wchar_t *kind, const wchar_t *text, DWORD extra, int cx, int cy, int cw, int ch,
                     int id) {
        HWND control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | extra, S(cx), S(cy), S(cw), S(ch),
                                       dlg.window, (HMENU)(INT_PTR)id, g.instance, NULL);
        if (dlg.font) SendMessageW(control, WM_SETFONT, (WPARAM)dlg.font, TRUE);
        return control;
    };
    for (int a = 0; a < GBI_COUNT; a++) {
        std::wstring name = widen(gbi_name((GbInput)a));
        child(L"STATIC", name.c_str(), SS_LEFT, 20, 21 + a * 32, 110, 20, -1);
        for (int k = 0; k < GBI_KEYS_PER_ACTION; k++)
            dlg.slot[a][k] = child(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 140 + k * 140, 16 + a * 32, 130, 28,
                                   ID_SLOT_BASE + a * GBI_KEYS_PER_ACTION + k);
    }
    child(L"STATIC",
          L"Controller: D-pad or left stick, face buttons, Start and Back work as on a Game Boy. "
          L"The right shoulder or a trigger fast-forwards. Hold the left shoulder with Start, Back or Y "
          L"to save, load or change slot.",
          SS_LEFT, 20, 408, 390, 54, -1);
    dlg.hint = child(L"STATIC", L"", SS_LEFT, 20, 468, 390, 20, -1);
    child(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, 20, 488, 140, 28, ID_RESET);
    dlg.done = child(L"BUTTON", L"Done", BS_DEFPUSHBUTTON | WS_TABSTOP, 300, 488, 110, 28, ID_DONE);
    refresh();

    bool was_open = g.dialog_open;
    g.dialog_open = true;
    release_keys();
    sync_audio();
    EnableWindow(g.hwnd, FALSE);
    ShowWindow(dlg.window, SW_SHOW);
    SetFocus(dlg.done);
    dlg.running = true;
    MSG msg;
    while (dlg.running) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)msg.wParam); /* the app is closing */
            break;
        }
        if (dlg.capture >= 0 && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
            (msg.hwnd == dlg.window || IsChild(dlg.window, msg.hwnd))) {
            capture_key(msg.lParam);
            continue;
        }
        if (IsDialogMessageW(dlg.window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(g.hwnd, TRUE);
    DestroyWindow(dlg.window);
    if (dlg.font) DeleteObject(dlg.font);
    dlg = Dialog();
    g.dialog_open = was_open;
    g.previous = SDL_GetTicksNS();
    SetFocus(g.hwnd);
    sync_audio();
    update_controls();
}

} // namespace controls

void show_controls() { controls::show(); }

void on_command(int id) {
    switch (id) {
    case IDM_OPEN: open_rom(); break;
    case IDM_DEMO: load_demo(); break;
    case IDM_EXIT: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
    case IDM_PAUSE: toggle_pause(); break;
    case IDM_RESTART: restart_rom(); break;
    case IDM_SOUND: toggle_sound(); break;
    case IDM_FULLSCREEN: toggle_fullscreen(); break;
    case IDM_ABOUT: show_about(); break;
    case IDM_SAVE_STATE: save_state(); SetFocus(g.hwnd); break;
    case IDM_LOAD_STATE: load_state(); SetFocus(g.hwnd); break;
    case IDM_CONTROLS: show_controls(); break;
    case IDCANCEL:
        if (g.fullscreen) toggle_fullscreen();
        break;
    case IDC_SPEED: choose_popup(g.speed, speed_names(), IDM_SPEED_BASE, speed_index(), 3); break;
    case IDC_PALETTE: choose_popup(g.palette_button, PALETTE_NAME, IDM_PALETTE_BASE, g.palette, 3); break;
    default:
        if (id >= IDM_SPEED_BASE && id < IDM_SPEED_BASE + 3) set_speed(id - IDM_SPEED_BASE);
        else if (id >= IDM_PALETTE_BASE && id < IDM_PALETTE_BASE + 3) set_palette(id - IDM_PALETTE_BASE);
        else if (id >= IDM_SLOT_BASE && id < IDM_SLOT_BASE + GB_STATE_SLOTS) {
            set_slot(id - IDM_SLOT_BASE);
            SetFocus(g.hwnd);
        }
        else if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + MAX_RECENT) {
            size_t index = (size_t)(id - IDM_RECENT_BASE);
            if (index < g.recents.size()) load_file(g.recents[index]);
        }
        break;
    }
}

/* ---- Keyboard ---------------------------------------------------------- */

/* Physical key positions from the scan code, independent of keyboard layout. */
int chip_key(UINT scan) {
    switch (scan) {
    case 0x02: return 1;  case 0x03: return 2;  case 0x04: return 3;  case 0x05: return 12;
    case 0x10: return 4;  case 0x11: return 5;  case 0x12: return 6;  case 0x13: return 13;
    case 0x1E: return 7;  case 0x1F: return 8;  case 0x20: return 9;  case 0x21: return 14;
    case 0x2C: return 10; case 0x2D: return 0;  case 0x2E: return 11; case 0x2F: return 15;
    default: return -1;
    }
}

void set_gb_key(int button, bool held) {
    g.gb_kb[button] = held;
    apply_buttons();
}

/* A bound Game Boy key was pressed or released. */
void gb_action_press(int action, bool repeat) {
    if (action < GBI_BUTTONS) {
        if (!paused() && !repeat) set_gb_key(action, true);
    } else if (action == GBI_FAST_FORWARD) {
        g.ff_key = true;
    } else if (!repeat) {
        if (action == GBI_SAVE_STATE) save_state();
        else if (action == GBI_LOAD_STATE) load_state();
        else if (action == GBI_NEXT_SLOT) set_slot((g.slot + 1) % GB_STATE_SLOTS);
    }
}

void gb_action_release(int action) {
    if (action < GBI_BUTTONS) set_gb_key(action, false);
    else if (action == GBI_FAST_FORWARD) g.ff_key = false;
}

void sync_key_display() {
    if (memcmp(g.shown_keys, g.machine.keys, sizeof(g.shown_keys)) != 0) {
        memcpy(g.shown_keys, g.machine.keys, sizeof(g.shown_keys));
        invalidate_keys();
    }
    if (memcmp(g.shown_gb_keys, g.gb_keys, sizeof(g.shown_gb_keys)) != 0) {
        memcpy(g.shown_gb_keys, g.gb_keys, sizeof(g.shown_gb_keys));
        invalidate_keys();
    }
}

bool handle_key(UINT message, WPARAM wparam, LPARAM lparam, bool main_has_focus) {
    bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return false;
    UINT scan = (lparam >> 16) & 0xFF;
    bool extended = (lparam & (1 << 24)) != 0;
    if (g.is_gb) {
        int action = gbi_lookup(&g.keys, key_code(scan, extended));
        if (up) {
            if (action < 0) return false;
            gb_action_release(action);
            sync_key_display();
            return true;
        }
        if (action >= 0 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {
            gb_action_press(action, (lparam & (1 << 30)) != 0);
            sync_key_display();
            return true;
        }
    }
    int key = extended || g.is_gb ? -1 : chip_key(scan);
    if (up) {
        if (key < 0) return false;
        chip8_set_key(&g.machine, (uint8_t)key, 0);
        sync_key_display();
        return true;
    }
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return false;
    bool repeat = (lparam & (1 << 30)) != 0;
    if (!repeat) {
        if (wparam == VK_SPACE && main_has_focus) {
            toggle_pause();
            return true;
        }
        if (scan == 0x32 && !extended) { /* M */
            toggle_sound();
            return true;
        }
    }
    if (key >= 0) {
        if (!paused() && !repeat) chip8_set_key(&g.machine, (uint8_t)key, 1);
        sync_key_display();
        return true;
    }
    return false;
}

/* ---- Drag and drop ----------------------------------------------------- */

bool has_files(IDataObject *data) {
    FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data && data->QueryGetData(&format) == S_OK;
}

class DropTarget : public IDropTarget {
public:
    STDMETHODIMP QueryInterface(REFIID id, void **object) override {
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *object = static_cast<IDropTarget *>(this);
            return S_OK;
        }
        *object = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP DragEnter(IDataObject *data, DWORD, POINTL, DWORD *effect) override {
        accepts = has_files(data);
        g.drop_active = accepts;
        invalidate_well();
        *effect = accepts ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD *effect) override {
        *effect = accepts ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        g.drop_active = false;
        invalidate_well();
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject *data, DWORD, POINTL, DWORD *effect) override {
        g.drop_active = false;
        invalidate_well();
        *effect = DROPEFFECT_NONE;
        FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        if (data->GetData(&format, &medium) != S_OK) return S_OK;
        HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
        /* One file at a time, like the Mac window. Load after the drag finishes. */
        if (drop && DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) == 1) {
            UINT length = DragQueryFileW(drop, 0, NULL, 0);
            std::wstring *path = new std::wstring(length, L'\0');
            DragQueryFileW(drop, 0, &(*path)[0], length + 1);
            if (PostMessageW(g.hwnd, WM_NACRE_LOAD, 0, (LPARAM)path)) *effect = DROPEFFECT_COPY;
            else delete path;
        }
        if (drop) GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return S_OK;
    }

private:
    bool accepts = false;
};

DropTarget drop_target;

/* ---- Frame loop -------------------------------------------------------- */

/* Reads the controllers: held buttons feed the console, and the state hotkeys fire once
 * per press. Only the focused window listens, so a game in the background isn't played. */
void poll_pad() {
    unsigned held = pad_poll();
    const char *note = pad_take_message();
    if (note && g.is_gb) set_status(widen(note));
    bool listening = g.is_gb && !g.dialog_open && GetForegroundWindow() == g.hwnd;
    if (!listening) held = 0;
    unsigned fresh = held & ~g.pad_seen;
    g.pad_seen = held;
    g.pad_held = held;
    apply_buttons();
    if (fresh & PAD_BIT(GBI_SAVE_STATE)) save_state();
    if (fresh & PAD_BIT(GBI_LOAD_STATE)) load_state();
    if (fresh & PAD_BIT(GBI_NEXT_SLOT)) set_slot((g.slot + 1) % GB_STATE_SLOTS);
}

void tick() {
    if (g.in_tick) return;
    g.in_tick = true;
    uint64_t now = SDL_GetTicksNS(), elapsed = now - g.previous;
    g.previous = now;
    char error[256];
    if (g.is_gb) {
        poll_pad();
        bool fast = (g.ff_key || (g.pad_held & PAD_BIT(GBI_FAST_FORWARD)) != 0) && !paused();
        if (fast != g.ff_on) {
            g.ff_on = fast;
            invalidate_well();
        }
        if (!g.status.empty() && now >= g.status_until) {
            g.status.clear();
            invalidate_footer();
        }
        unsigned percent = g.ff_on ? GB_FAST_PERCENT : GB_SPEED_PERCENT[g.gb_speed];
        if (!paused() && !gb_advance_ns(g.gb, elapsed, percent, error, sizeof(error))) {
            g.fault = error;
            write_save();
            release_keys();
            update_controls();
            show_alert(widen(error));
        }
        sync_audio();
        pump_gb_audio();
        sync_key_display();
        if (gb_frame_count(g.gb) != g.gb_frame_seen) invalidate_well();
        /* Games write their save RAM in bursts; settle for a second before writing. */
        if (!g.save_path.empty() && gb_save_dirty(g.gb)) {
            if (!g.save_due) g.save_due = now + 1000000000u;
            else if (now >= g.save_due) write_save();
        }
        g.in_tick = false;
        return;
    }
    if (g.ff_on) {
        g.ff_on = false;
        invalidate_well();
    }
    if (!chip8_scheduler_advance(&g.machine, &g.scheduler, elapsed, paused(), timer_tick, NULL, error,
                                 sizeof(error))) {
        g.fault = error;
        release_keys();
        update_controls();
        show_alert(widen(error));
    }
    sync_audio();
    sync_key_display();
    if (memcmp(g.shown, g.machine.display, sizeof(g.shown)) != 0) invalidate_well();
    g.in_tick = false;
}

/* ---- Self-check and capture (--snapshot) ------------------------------- */

bool png_clsid(CLSID *clsid) {
    UINT count = 0, size = 0;
    GetImageEncodersSize(&count, &size);
    if (!size) return false;
    std::vector<BYTE> buffer(size);
    ImageCodecInfo *codecs = reinterpret_cast<ImageCodecInfo *>(buffer.data());
    GetImageEncoders(count, size, codecs);
    for (UINT i = 0; i < count; i++) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

bool capture_client(const std::wstring &path) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bitmap);
    UpdateWindow(g.hwnd);
    bool ok = PrintWindow(g.hwnd, mem, PW_CLIENTONLY) != 0;
    SelectObject(mem, old);
    CLSID png;
    if (ok && png_clsid(&png)) {
        Bitmap image(bitmap, NULL);
        ok = image.Save(path.c_str(), &png, NULL) == Ok;
    } else {
        ok = false;
    }
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return ok;
}

/* A minimal 32 KB cartridge: a valid header and a JR -2 loop at the entry point,
 * so the Game Boy path can be checked without shipping a ROM. */
std::vector<uint8_t> snapshot_cartridge() {
    static const uint8_t logo[48] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    };
    std::vector<uint8_t> rom(0x8000, 0);
    memcpy(&rom[0x104], logo, sizeof(logo));
    memcpy(&rom[0x134], "NACRECHECK", 10);
    rom[0x100] = 0x18; /* JR -2 */
    rom[0x101] = 0xFE;
    return rom;
}

/* Presses and releases Right through the window's key path, then checks frames
 * advance. Returns 0, or base..base+2 for the failing step. */
int check_gb_keys(int base) {
    const LPARAM right = (LPARAM)(1 | (0x4D << 16) | (1 << 24)); /* extended scan code */
    SendMessageW(g.hwnd, WM_KEYDOWN, VK_RIGHT, right);
    if (!g.gb_keys[GB_BUTTON_RIGHT]) return base;
    SendMessageW(g.hwnd, WM_KEYUP, VK_RIGHT, right | (LPARAM)(3u << 30));
    if (g.gb_keys[GB_BUTTON_RIGHT]) return base + 1;
    char error[256];
    uint32_t frames = gb_frame_count(g.gb);
    if (!gb_run_cycles(g.gb, GB_CLOCK_HZ / 5, error, sizeof(error)) || gb_frame_count(g.gb) - frames < 10)
        return base + 2;
    return 0;
}

/* Fast-forward as a held key, a save-state round trip on the running cartridge, and the
 * rebinding table. Returns 0, or base..base+8 for the failing step. */
int check_gb_features(int base) {
    const LPARAM tab = (LPARAM)(1 | (0x0F << 16));
    SendMessageW(g.hwnd, WM_KEYDOWN, VK_TAB, tab);
    if (!g.ff_key) return base;
    SendMessageW(g.hwnd, WM_KEYUP, VK_TAB, tab | (LPARAM)(3u << 30));
    if (g.ff_key) return base + 1;

    char error[256];
    std::vector<uint8_t> blob(gb_state_size(g.gb));
    if (blob.empty() || gb_state_save(g.gb, blob.data(), blob.size()) != blob.size()) return base + 2;
    uint32_t saved = gb_frame_count(g.gb);
    if (!gb_run_cycles(g.gb, GB_CLOCK_HZ / 10, error, sizeof(error)) || gb_frame_count(g.gb) == saved)
        return base + 3;
    if (!gb_state_load(g.gb, blob.data(), blob.size())) return base + 4;
    if (gb_frame_count(g.gb) != saved) return base + 5;
    blob[blob.size() / 2] ^= 0xFF;
    blob[0] ^= 0xFF; /* a damaged header is refused */
    if (gb_state_load(g.gb, blob.data(), blob.size())) return base + 6;

    GbBindings original = g.keys;
    if (gbi_assign(&g.keys, GBI_A, 0, 0x0F) != GBI_FAST_FORWARD || g.keys.key[GBI_FAST_FORWARD][0] != -1)
        return base + 7;
    char text[256];
    GbBindings parsed;
    bool round_trip = gbi_serialize(&g.keys, text, sizeof(text)) > 0 && gbi_parse(&parsed, text) &&
                      memcmp(&parsed, &g.keys, sizeof(parsed)) == 0;
    g.keys = original;
    return round_trip ? 0 : base + 8;
}

/* Exercise native actions and the physical keypad path before rendering, like
 * the Mac snapshot. Returns 0 on success or a nonzero stage number. */
int run_snapshot() {
    bool original_mute = g.muted;
    SendMessageW(g.hwnd, WM_COMMAND, IDM_PAUSE, 0);
    if (!g.user_paused) return 2;
    SendMessageW(g.hwnd, WM_COMMAND, IDM_PAUSE, 0);
    SendMessageW(g.hwnd, WM_COMMAND, IDM_SOUND, 0);
    SendMessageW(g.hwnd, WM_COMMAND, IDM_SOUND, 0);
    if (g.muted != original_mute) return 3;
    SendMessageW(g.hwnd, WM_COMMAND, IDM_RESTART, 0);
    if (g.rom.empty() || !g.fault.empty()) return 4;
    g.focus_paused = false;
    char error[256];
    if (g.is_gb) {
        /* Launched with a cartridge: check its keys, run it a while, and capture it. */
        int stage = check_gb_keys(12);
        if (!stage) stage = check_gb_features(16);
        if (stage) return stage;
        if (!gb_run_cycles(g.gb, (uint32_t)(GB_CLOCK_HZ * g.snapshot_seconds), error, sizeof(error)))
            return 15;
        invalidate_all();
        return capture_client(g.snapshot_path) ? 0 : 1;
    }
    /* The X key sits at physical scan code 0x2D and maps to CHIP-8 key 0. */
    SendMessageW(g.hwnd, WM_KEYDOWN, 'X', (LPARAM)(1 | (0x2D << 16)));
    if (!g.machine.keys[0]) return 5;
    SendMessageW(g.hwnd, WM_KEYUP, 'X', (LPARAM)(1 | (0x2D << 16) | (3u << 30)));
    if (g.machine.keys[0]) return 6;

    /* The Game Boy path on a stand-in cartridge, then back to the CHIP-8 ROM. */
    std::vector<uint8_t> rom = g.rom;
    std::wstring rom_path = g.rom_path;
    bool bundled = g.rom_bundled;
    load_candidate(snapshot_cartridge(), L"", false);
    if (!g.is_gb || !g.fault.empty()) return 7;
    int stage = check_gb_keys(8);
    if (!stage) stage = check_gb_features(30);
    if (stage) return stage;
    load_candidate(rom, rom_path, bundled);
    if (g.is_gb) return 11;

    chip8_scheduler_advance(&g.machine, &g.scheduler, 200000000, 0, NULL, NULL, error, sizeof(error));
    invalidate_all();
    return capture_client(g.snapshot_path) ? 0 : 1;
}

/* ---- Window ------------------------------------------------------------ */

void build_menus() {
    g.menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open ROM…\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_DEMO, L"Open &welcome demo");
    g.recent_menu = CreatePopupMenu();
    AppendMenuW(file, MF_POPUP, (UINT_PTR)g.recent_menu, L"Open &Recent");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit\tCtrl+Q");
    AppendMenuW(g.menu, MF_POPUP, (UINT_PTR)file, L"&File");

    g.session_menu = CreatePopupMenu();
    AppendMenuW(g.session_menu, MF_STRING, IDM_PAUSE, L"&Pause / Resume\tCtrl+P");
    AppendMenuW(g.session_menu, MF_STRING, IDM_RESTART, L"&Restart ROM\tCtrl+R");
    AppendMenuW(g.session_menu, MF_STRING, IDM_SOUND, L"Toggle &Sound\tM");
    AppendMenuW(g.session_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g.session_menu, MF_STRING, IDM_SAVE_STATE, L"Sa&ve state");
    AppendMenuW(g.session_menu, MF_STRING, IDM_LOAD_STATE, L"&Load state");
    g.slot_menu = CreatePopupMenu();
    for (int i = 0; i < GB_STATE_SLOTS; i++) AppendMenuW(g.slot_menu, MF_STRING, IDM_SLOT_BASE + i, L"Slot");
    AppendMenuW(g.session_menu, MF_POPUP, (UINT_PTR)g.slot_menu, L"S&tate slot");
    AppendMenuW(g.session_menu, MF_STRING, IDM_CONTROLS, L"&Controls…");
    AppendMenuW(g.session_menu, MF_SEPARATOR, 0, NULL);
    g.speed_menu = CreatePopupMenu();
    for (int i = 0; i < 3; i++) AppendMenuW(g.speed_menu, MF_STRING, IDM_SPEED_BASE + i, SPEED_NAME[i]);
    AppendMenuW(g.session_menu, MF_POPUP, (UINT_PTR)g.speed_menu, L"Execution s&peed");
    AppendMenuW(g.menu, MF_POPUP, (UINT_PTR)g.session_menu, L"&Session");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_FULLSCREEN, L"Toggle &Full Screen\tF11");
    g.palette_menu = CreatePopupMenu();
    for (int i = 0; i < 3; i++)
        AppendMenuW(g.palette_menu, MF_STRING, IDM_PALETTE_BASE + i, PALETTE_NAME[i]);
    AppendMenuW(view, MF_POPUP, (UINT_PTR)g.palette_menu, L"&Display palette");
    AppendMenuW(g.menu, MF_POPUP, (UINT_PTR)view, L"&View");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About Nacre");
    AppendMenuW(g.menu, MF_POPUP, (UINT_PTR)help, L"&Help");

    ACCEL keys[] = {
        {FCONTROL | FVIRTKEY, 'O', IDM_OPEN},    {FCONTROL | FVIRTKEY, 'P', IDM_PAUSE},
        {FCONTROL | FVIRTKEY, 'R', IDM_RESTART}, {FVIRTKEY, VK_F2, IDM_RESTART},
        {FVIRTKEY, VK_F11, IDM_FULLSCREEN},      {FALT | FVIRTKEY, VK_RETURN, IDM_FULLSCREEN},
        {FCONTROL | FVIRTKEY, 'Q', IDM_EXIT},
    };
    g.accel = CreateAcceleratorTableW(keys, (int)(sizeof(keys) / sizeof(keys[0])));
}

void refresh_menu(HMENU menu) {
    if (menu == g.recent_menu) {
        while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
        for (size_t i = 0; i < g.recents.size(); i++) {
            std::wstring label = L"&" + std::to_wstring(i + 1 < 10 ? i + 1 : 0) + L"  " +
                                 file_name(g.recents[i]);
            if (i >= 9) label = file_name(g.recents[i]);
            AppendMenuW(menu, MF_STRING, (UINT_PTR)(IDM_RECENT_BASE + (int)i), label.c_str());
        }
        if (g.recents.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No recent ROMs");
    } else if (menu == g.session_menu) {
        bool ready = !g.rom.empty();
        EnableMenuItem(menu, IDM_PAUSE, MF_BYCOMMAND | (ready && g.fault.empty() ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menu, IDM_RESTART, MF_BYCOMMAND | (ready ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menu, IDM_SOUND, MF_BYCOMMAND | (g.audio_available ? MF_ENABLED : MF_GRAYED));
        ModifyMenuW(menu, IDM_PAUSE, MF_BYCOMMAND | MF_STRING, IDM_PAUSE,
                    g.user_paused ? L"&Resume\tCtrl+P" : L"&Pause\tCtrl+P");
        CheckMenuItem(menu, IDM_SOUND, MF_BYCOMMAND | (g.muted ? MF_UNCHECKED : MF_CHECKED));
        bool states = g.is_gb && ready && !state_path(g.slot).empty();
        UINT can = states ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(menu, IDM_SAVE_STATE, MF_BYCOMMAND | can);
        EnableMenuItem(menu, IDM_LOAD_STATE, MF_BYCOMMAND | can);
        EnableMenuItem(menu, 6, MF_BYPOSITION | (g.is_gb && ready ? MF_ENABLED : MF_GRAYED));
        ModifyMenuW(menu, IDM_SAVE_STATE, MF_BYCOMMAND | MF_STRING, IDM_SAVE_STATE,
                    menu_label(L"Sa&ve state", GBI_SAVE_STATE).c_str());
        ModifyMenuW(menu, IDM_LOAD_STATE, MF_BYCOMMAND | MF_STRING, IDM_LOAD_STATE,
                    menu_label(L"&Load state", GBI_LOAD_STATE).c_str());
    } else if (menu == g.slot_menu) {
        for (int i = 0; i < GB_STATE_SLOTS; i++)
            ModifyMenuW(menu, (UINT)i, MF_BYPOSITION | MF_STRING, (UINT_PTR)(IDM_SLOT_BASE + i),
                        slot_label(i).c_str());
        CheckMenuRadioItem(menu, 0, GB_STATE_SLOTS - 1, (UINT)g.slot, MF_BYPOSITION);
    } else if (menu == g.speed_menu) {
        for (int i = 0; i < 3; i++)
            ModifyMenuW(menu, (UINT)i, MF_BYPOSITION | MF_STRING, (UINT_PTR)(IDM_SPEED_BASE + i),
                        speed_names()[i]);
        CheckMenuRadioItem(menu, 0, 2, (UINT)speed_index(), MF_BYPOSITION);
    } else if (menu == g.palette_menu) {
        CheckMenuRadioItem(menu, 0, 2, (UINT)g.palette, MF_BYPOSITION);
    }
}

void style_title_bar() {
    /* Windows 11 only; older builds ignore these attributes. */
    COLORREF caption = RGB(0xF5, 0xF3, 0xEE), text = RGB(0x29, 0x3D, 0x34);
    BOOL dark = FALSE;
    DwmSetWindowAttribute(g.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DwmSetWindowAttribute(g.hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(g.hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
}

void apply_client_size(int client_w, int client_h) {
    UINT dpi = GetDpiForWindow(g.hwnd);
    RECT rc = {0, 0, MulDiv(client_w, (int)dpi, 96), MulDiv(client_h, (int)dpi, 96)};
    DWORD style = (DWORD)GetWindowLongPtrW(g.hwnd, GWL_STYLE);
    AdjustWindowRectExForDpi(&rc, style, TRUE, 0, dpi);
    SetWindowPos(g.hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool placement_is_visible(const WINDOWPLACEMENT &placement) {
    return MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONULL) != NULL;
}

void save_placement() {
    if (g.fullscreen || g.ephemeral) return;
    WINDOWPLACEMENT placement = {sizeof(placement)};
    if (GetWindowPlacement(g.hwnd, &placement))
        reg_set(L"Placement", REG_BINARY, &placement, sizeof(placement));
}

bool restore_placement() {
    WINDOWPLACEMENT placement = {};
    DWORD size = sizeof(placement);
    if (g.ephemeral ||
        RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Placement", RRF_RT_REG_BINARY, NULL, &placement,
                     &size) != ERROR_SUCCESS ||
        size != sizeof(placement) || placement.length != sizeof(placement) ||
        !placement_is_visible(placement))
        return false;
    placement.showCmd = placement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    return SetWindowPlacement(g.hwnd, &placement) != 0;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_to(dc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT all = {0, 0, g.back_w, g.back_h};
        paint_to(reinterpret_cast<HDC>(wparam), all);
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g.scale = (float)GetDpiForWindow(hwnd) / 96.0f;
            ensure_back_buffer(LOWORD(lparam), HIWORD(lparam));
            layout();
            invalidate_all();
        }
        return 0;
    case WM_DPICHANGED: {
        g.scale = (float)HIWORD(wparam) / 96.0f;
        const RECT *suggested = reinterpret_cast<const RECT *>(lparam);
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = reinterpret_cast<MINMAXINFO *>(lparam);
        UINT dpi = GetDpiForWindow(hwnd);
        RECT rc = {0, 0, MulDiv(MIN_W, (int)dpi, 96), MulDiv(MIN_H, (int)dpi, 96)};
        AdjustWindowRectExForDpi(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), g.menu && !g.fullscreen,
                                 0, dpi);
        info->ptMinTrackSize.x = rc.right - rc.left;
        info->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }
    case WM_DRAWITEM:
        draw_button(reinterpret_cast<const DRAWITEMSTRUCT *>(lparam));
        return TRUE;
    case WM_COMMAND:
        if (lparam == 0 || HIWORD(wparam) == BN_CLICKED) on_command((int)LOWORD(wparam));
        return 0;
    case WM_INITMENUPOPUP:
        refresh_menu(reinterpret_cast<HMENU>(wparam));
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (handle_key(message, wparam, lparam, true)) return 0;
        break;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            g.focus_paused = true;
            release_keys();
            sync_key_display();
        } else {
            g.focus_paused = false;
            g.previous = SDL_GetTicksNS();
        }
        sync_audio();
        invalidate_header();
        invalidate_well();
        break;
    case WM_SETFOCUS:
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_TICK) {
            tick();
        } else if (wparam == TIMER_SNAPSHOT) {
            KillTimer(hwnd, TIMER_SNAPSHOT);
            PostMessageW(hwnd, WM_NACRE_SNAPSHOT, 0, 0);
        }
        return 0;
    case WM_NACRE_SNAPSHOT:
        g.snapshot_status = run_snapshot();
        DestroyWindow(hwnd);
        return 0;
    case WM_NACRE_LOAD: {
        std::wstring *path = reinterpret_cast<std::wstring *>(lparam);
        load_file(*path);
        delete path;
        return 0;
    }
    case WM_CLOSE:
        write_save();
        save_placement();
        DestroyWindow(hwnd);
        return 0;
    case WM_ENDSESSION:
        if (wparam) write_save();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        RevokeDragDrop(hwnd);
        PostQuitMessage(g.snapshot_status);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* Game keys must work even when a sidebar button holds keyboard focus, and must
 * reach the game before dialog navigation claims arrows, Enter and Backspace. */
bool route_game_key(const MSG &msg) {
    if (msg.hwnd != g.hwnd && !IsChild(g.hwnd, msg.hwnd)) return false;
    if (msg.message != WM_KEYDOWN && msg.message != WM_KEYUP && msg.message != WM_SYSKEYDOWN &&
        msg.message != WM_SYSKEYUP)
        return false;
    return handle_key(msg.message, msg.wParam, msg.lParam, msg.hwnd == g.hwnd);
}

bool initialize(int show) {
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);

    g.text_font = family_exists(L"Segoe UI") ? L"Segoe UI" : L"Arial";
    g.semibold_font = family_exists(L"Segoe UI Semibold") ? L"Segoe UI Semibold" : g.text_font;
    g.mono_font = family_exists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    if (family_exists(L"Segoe Fluent Icons")) g.icon_font = L"Segoe Fluent Icons";
    else if (family_exists(L"Segoe MDL2 Assets")) g.icon_font = L"Segoe MDL2 Assets";

    g.muted = reg_dword(L"muted", 0) != 0;
    g.palette = (int)reg_dword(L"palette", 0);
    if (g.palette < 0 || g.palette > 2) g.palette = 0;
    g.cpu_hz = reg_dword(L"cpuHz", CHIP8_DEFAULT_CPU_HZ);
    if (g.cpu_hz != 300 && g.cpu_hz != 600 && g.cpu_hz != 1000) g.cpu_hz = CHIP8_DEFAULT_CPU_HZ;
    g.gb_speed = (int)reg_dword(L"gbSpeed", 1);
    if (g.gb_speed < 0 || g.gb_speed > 2) g.gb_speed = 1;
    load_bindings();
    if (!g.ephemeral) g.slot = (int)std::min<DWORD>(reg_dword(L"gbSlot", 0), (DWORD)(GB_STATE_SLOTS - 1));
    g.gb = gb_create();
    if (!g.ephemeral) load_recents();
    chip8_init(&g.machine, 0x4E414352u);
    chip8_scheduler_init(&g.scheduler, g.cpu_hz);

    HICON icon_big = static_cast<HICON>(LoadImageW(g.instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    HICON icon_small = static_cast<HICON>(LoadImageW(g.instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), 0));
    WNDCLASSEXW cls = {sizeof(cls)};
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = g.instance;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = icon_big;
    cls.hIconSm = icon_small;
    cls.lpszClassName = L"NacreWindow";
    if (!RegisterClassExW(&cls)) return false;

    build_menus();
    g.hwnd = CreateWindowExW(0, cls.lpszClassName, L"Nacre", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, g.menu, g.instance, NULL);
    if (!g.hwnd) return false;
    g.scale = (float)GetDpiForWindow(g.hwnd) / 96.0f;
    style_title_bar();

    g.tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g.hwnd,
                                 NULL, g.instance, NULL);
    g.open = make_button(IDM_OPEN, L"Open a CHIP-8 or Game Boy ROM (Ctrl+O)");
    g.pause = make_button(IDM_PAUSE, L"Pause or resume (Space)");
    g.restart = make_button(IDM_RESTART, L"Restart the loaded ROM (Ctrl+R)");
    g.sound = make_button(IDM_SOUND, L"Toggle sound (M)");
    g.speed = make_button(IDC_SPEED, L"Execution speed");
    g.palette_button = make_button(IDC_PALETTE, L"Display palette");

    apply_client_size(CLIENT_W, CLIENT_H);
    if (!restore_placement()) {
        RECT rc;
        GetWindowRect(g.hwnd, &rc);
        MONITORINFO info = {sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTOPRIMARY), &info);
        int x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - (rc.right - rc.left)) / 2;
        int y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - (rc.bottom - rc.top)) / 2;
        SetWindowPos(g.hwnd, NULL, std::max(x, (int)info.rcWork.left), std::max(y, (int)info.rcWork.top),
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RegisterDragDrop(g.hwnd, &drop_target);

    chip8_audio_oscillator_reset(&g.audio.oscillator);
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, CHIP8_AUDIO_SAMPLE_RATE};
    g.audio.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                               audio_callback, &g.audio);
    g.audio_available = g.audio.stream && SDL_ResumeAudioStreamDevice(g.audio.stream);
    /* The Game Boy pushes stereo samples itself; its stream stays paused until a
     * cartridge runs. */
    SDL_AudioSpec gb_spec = {SDL_AUDIO_S16, 2, (int)GB_AUDIO_SAMPLE_RATE};
    g.gb_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &gb_spec, NULL, NULL);

    ShowWindow(g.hwnd, show);
    return true;
}

void shutdown() {
    write_save();
    SDL_SetAtomicInt(&g.audio.active, 0);
    if (g.audio.stream) SDL_DestroyAudioStream(g.audio.stream);
    if (g.gb_stream) SDL_DestroyAudioStream(g.gb_stream);
    delete g.gb_bitmap;
    g.gb_bitmap = NULL;
    gb_destroy(g.gb);
    g.gb = NULL;
    for (auto &entry : g.fonts) delete entry.second;
    g.fonts.clear();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g.instance = instance;
    std::wstring rom_argument;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g.snapshot_seconds = 0.2;
    for (int i = 1; argv && i < argc; i++) {
        if (wcscmp(argv[i], L"--snapshot") == 0 && i + 1 < argc) {
            g.snapshot_path = argv[++i];
            g.ephemeral = true;
        } else if (wcscmp(argv[i], L"--seconds") == 0 && i + 1 < argc) {
            g.snapshot_seconds = std::min(std::max(_wtof(argv[++i]), 0.0), 600.0);
        } else {
            rom_argument = argv[i];
        }
    }
    if (argv) LocalFree(argv);

    if (FAILED(OleInitialize(NULL))) return 1;
    GdiplusStartupInput input;
    ULONG_PTR gdiplus_token = 0;
    if (GdiplusStartup(&gdiplus_token, &input, NULL) != Ok) return 1;
    SDL_Init(SDL_INIT_AUDIO);
    pad_init();
    timeBeginPeriod(1);

    int status = 1;
    if (initialize(g.snapshot_path.empty() ? show : SW_SHOWNORMAL)) {
        if (!rom_argument.empty()) load_file(rom_argument);
        if (g.rom.empty()) load_demo();
        update_controls();
        g.previous = SDL_GetTicksNS();
        SetFocus(g.hwnd);
        SetTimer(g.hwnd, TIMER_TICK, 16, NULL);
        if (!g.snapshot_path.empty()) SetTimer(g.hwnd, TIMER_SNAPSHOT, 500, NULL);

        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (g.accel && TranslateAcceleratorW(g.hwnd, g.accel, &msg)) continue;
            if (route_game_key(msg)) continue;
            if (IsDialogMessageW(g.hwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        status = (int)msg.wParam;
        shutdown();
    }
    timeEndPeriod(1);
    pad_shutdown();
    SDL_Quit();
    GdiplusShutdown(gdiplus_token);
    OleUninitialize();
    return status;
}
