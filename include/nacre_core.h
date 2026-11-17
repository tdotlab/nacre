#ifndef NACRE_CORE_H
#define NACRE_CORE_H

/* The system-agnostic core interface. Every emulated system (CHIP-8, Game Boy, and
 * later ones, including forked emulators that run their own threads and draw into
 * a native window) is reached through one NacreCore table. Frontends and the
 * session layer talk only to this table, never to a system's own header.
 *
 * Two run models share the table:
 *
 *   Stepped cores (the default) do nothing on their own. The host calls `advance`
 *   once per display refresh with the wall time that passed; the core runs that
 *   much emulated time and returns.
 *
 *   Threaded cores (NACRE_CORE_THREADED) run on threads they own. The host starts
 *   and stops them with `set_running`; `advance` is NULL.
 *
 * Video is either a framebuffer the host draws (`video`), or, for cores flagged
 * NACRE_CORE_NATIVE_VIDEO, a native surface the host lends with `attach_surface`
 * and the core presents into itself.
 *
 * Threading contract: the host makes every call on one thread (its UI thread),
 * never two at once. A threaded core does its own locking. The only thing a core
 * may call from another thread is NacreHost.wake.
 *
 * Plain C99, no platform headers. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NACRE_CORE_API_VERSION 1

/* Probes see at most this many leading bytes of a file (enough to reach the ISO
 * 9660 volume descriptor at 32 KiB). */
#define NACRE_PROBE_BYTES 65536u

/* ---- Descriptor flags -------------------------------------------------- */

enum {
    NACRE_CORE_THREADED = 1u << 0,      /* runs on its own threads; uses set_running */
    NACRE_CORE_NATIVE_VIDEO = 1u << 1,  /* presents into an attached native surface */
    NACRE_CORE_SINGLETON = 1u << 2,     /* at most one live instance per process */
    NACRE_CORE_NEEDS_PATH = 1u << 3,    /* content must be loaded by path, not bytes */
    NACRE_CORE_NEEDS_SYSTEM_FILES = 1u << 4, /* needs BIOS or firmware in system_dir */
    NACRE_CORE_HAS_STATES = 1u << 5,    /* supports save states */
    NACRE_CORE_HAS_BATTERY = 1u << 6    /* may carry battery-backed saves */
};

/* ---- Video ------------------------------------------------------------- */

typedef enum {
    /* One byte per pixel holding a shade index 0 .. levels-1. 0 is the blank
     * shade: an unlit pixel on a two-level display (CHIP-8), the lightest shade
     * of an LCD (Game Boy). The host owns the palette. */
    NACRE_PIXEL_INDEX8 = 0,
    /* 0x00RRGGBB per pixel in native-endian uint32_t. */
    NACRE_PIXEL_XRGB8888 = 1
} NacrePixelFormat;

typedef struct {
    NacrePixelFormat format;
    unsigned width;
    unsigned height;
    size_t pitch;          /* bytes from one row to the next */
    const void *pixels;
    unsigned levels;       /* INDEX8: number of shades (2, 4, ...); 0 otherwise */
    float aspect;          /* display width / height; 0 means square pixels */
    uint32_t frame_count;  /* changes whenever the pixels change */
} NacreVideoFrame;

typedef enum {
    NACRE_SURFACE_WIN32_HWND = 1,   /* handle: HWND */
    NACRE_SURFACE_COCOA_NSVIEW = 2, /* handle: NSView * */
    NACRE_SURFACE_X11_WINDOW = 3,   /* handle: Window (cast), display: Display * */
    NACRE_SURFACE_WAYLAND = 4       /* handle: wl_surface *, display: wl_display * */
} NacreSurfaceKind;

typedef struct {
    NacreSurfaceKind kind;
    void *handle;
    void *display;
    unsigned width;   /* size in physical pixels */
    unsigned height;
    float scale;      /* physical pixels per logical point */
} NacreSurface;

/* ---- Input ------------------------------------------------------------- */

/* A standard controller. Face buttons are named by position, so a system maps
 * its own labels onto them (Game Boy A is EAST, B is SOUTH; PlayStation Cross is
 * SOUTH, Circle EAST). Each core lists the buttons it uses in `pad_labels`. */
typedef enum {
    NACRE_PAD_UP = 0,
    NACRE_PAD_DOWN,
    NACRE_PAD_LEFT,
    NACRE_PAD_RIGHT,
    NACRE_PAD_SOUTH,
    NACRE_PAD_EAST,
    NACRE_PAD_WEST,
    NACRE_PAD_NORTH,
    NACRE_PAD_L1,
    NACRE_PAD_R1,
    NACRE_PAD_L2,
    NACRE_PAD_R2,
    NACRE_PAD_L3,
    NACRE_PAD_R3,
    NACRE_PAD_SELECT,
    NACRE_PAD_START,
    NACRE_PAD_BUTTON_COUNT
} NacrePadButton;

#define NACRE_PAD_BIT(button) (1u << (button))

typedef enum {
    NACRE_AXIS_LEFT_X = 0,  /* -32768 left .. 32767 right */
    NACRE_AXIS_LEFT_Y,      /* -32768 up .. 32767 down */
    NACRE_AXIS_RIGHT_X,
    NACRE_AXIS_RIGHT_Y,
    NACRE_AXIS_COUNT
} NacreAxis;

/* ---- Host services ----------------------------------------------------- */

typedef enum {
    NACRE_LOG_DEBUG = 0,
    NACRE_LOG_INFO,
    NACRE_LOG_WARN,
    NACRE_LOG_ERROR
} NacreLogLevel;

typedef struct {
    /* UTF-8 directories the core may read (BIOS, firmware) and write (its own
     * memory cards and the like). Either may be NULL. */
    const char *system_dir;
    const char *save_dir;
    void (*log)(void *userdata, NacreLogLevel level, const char *message);
    /* Something changed that the host should look at (a new frame, an error, a
     * stop). Threaded cores call it from any thread; the host must only post a
     * note to its UI thread here. May be NULL. */
    void (*wake)(void *userdata);
    void *userdata;
} NacreHost;

typedef struct {
    const uint8_t *data;  /* whole file; NULL for NACRE_CORE_NEEDS_PATH cores */
    size_t size;
    const char *path;     /* UTF-8, may be NULL for bytes-only content */
} NacreContent;

/* ---- The core table ---------------------------------------------------- */

typedef struct NacreCore NacreCore;

struct NacreCore {
    unsigned api_version;   /* NACRE_CORE_API_VERSION */
    const char *id;         /* stable short name: "chip8", "gb" */
    const char *name;       /* display name: "CHIP-8", "Game Boy" */
    const char *extensions; /* for file dialogs, ';'-separated, no dots: "gb;dmg" */
    uint32_t flags;
    unsigned keypad_keys;   /* hex keypad size (16 for CHIP-8), 0 if none */
    /* Label for each pad button the system has; NULL where it has none. */
    const char *pad_labels[NACRE_PAD_BUTTON_COUNT];

    /* How sure the core is that it owns this file: 0 = not mine, 100 = certain.
     * `head` holds the first min(size, NACRE_PROBE_BYTES) bytes; `path` may be NULL. */
    int (*probe)(const uint8_t *head, size_t head_size, size_t size, const char *path);

    /* Instances. `host` is copied. Returns NULL with a message on failure. */
    void *(*create)(const NacreHost *host, char *error, size_t error_capacity);
    void (*destroy)(void *self);

    /* Loads content into a fresh instance; call once per instance. Returns 1, or
     * 0 with a message (the instance is then unusable and should be destroyed). */
    int (*load)(void *self, const NacreContent *content, char *error, size_t error_capacity);

    /* Power cycle with the same content. Clears a fault. */
    void (*reset)(void *self);

    /* Stepped cores: run `elapsed_ns` of wall time at the current speed. Hosts
     * call it every refresh, paused or not; a stopped core discards the time.
     * Returns 0 on the call where a fault happens (see `error`); the core then
     * halts, and later calls return 1 without running, until reset. NULL for
     * threaded cores. */
    int (*advance)(void *self, uint64_t elapsed_ns);

    /* Start or stop emulation. Stepped cores just stop counting time. Threaded
     * cores start or park their threads; stopping returns once they are parked. */
    void (*set_running)(void *self, int running);

    /* 100 = real speed. Applies to emulated time, not to option values. */
    void (*set_speed)(void *self, unsigned percent);

    /* Last fault, or NULL while healthy. */
    const char *(*error)(void *self);

    /* The latest complete frame, valid until the next call into the core. NULL
     * for native-video cores, or before content is loaded. */
    const NacreVideoFrame *(*video)(void *self);

    /* Native video only (otherwise NULL). Attach before set_running(1); a NULL
     * surface detaches. Call again with the new size after a resize. Returns 0
     * with a message if the surface can't be used. */
    int (*attach_surface)(void *self, const NacreSurface *surface, char *error,
                          size_t error_capacity);

    /* Audio: interleaved stereo int16 at `audio_rate` frames per second, produced
     * in step with emulated time. Returns frames copied. Cores keep a short
     * buffer (around a third of a second) and drop the oldest when it overflows. */
    unsigned (*audio_rate)(void *self);
    size_t (*audio_read)(void *self, int16_t *stereo, size_t max_frames);
    void (*audio_clear)(void *self);

    /* Input. `port` counts from 0. Buttons are NACRE_PAD_BIT flags; the full held
     * set replaces the previous one. set_pad may be NULL when no pad_labels are
     * set, set_axis for systems without sticks, set_keypad when keypad_keys is 0. */
    void (*set_pad)(void *self, unsigned port, uint32_t buttons);
    void (*set_axis)(void *self, unsigned port, NacreAxis axis, int16_t value);
    void (*set_keypad)(void *self, unsigned key, int pressed);

    /* Battery saves (cartridge RAM and the like) that the host keeps beside the
     * content. Size 0 means there is none. A core that keeps its own save files in
     * save_dir (memory cards) reports 0 here. All may be NULL together. */
    size_t (*save_size)(void *self);
    size_t (*save_write)(void *self, uint8_t *out, size_t capacity);
    int (*save_load)(void *self, const uint8_t *data, size_t size);
    int (*save_dirty)(void *self);
    void (*save_mark_clean)(void *self);
    /* Unix seconds for real-time clocks; may be NULL. */
    void (*set_wall_clock)(void *self, int64_t unix_seconds);

    /* Save states (NACRE_CORE_HAS_STATES). Size 0 means a state can't be taken
     * now. Save returns bytes written or 0. Load returns 1, or 0 and leaves the
     * machine untouched. Threaded cores pause around these internally. */
    size_t (*state_size)(void *self);
    size_t (*state_save)(void *self, uint8_t *out, size_t capacity);
    int (*state_load)(void *self, const uint8_t *data, size_t size);

    /* Named settings, values as text ("cpu_hz" = "600"). Returns 1 if taken. May
     * be NULL. */
    int (*set_option)(void *self, const char *key, const char *value);

    /* Game title from the content, or NULL. */
    const char *(*title)(void *self);
};

/* ---- Registry ---------------------------------------------------------- */

/* The built-in cores, NULL-terminated. */
const NacreCore *const *nacre_cores(void);
const NacreCore *nacre_core_by_id(const char *id);

/* The core with the highest probe score above 0, or NULL. `head` follows the
 * probe rules above. */
const NacreCore *nacre_core_detect(const uint8_t *head, size_t head_size, size_t size,
                                   const char *path);

/* Checks that a table has every entry its flags require. Returns 1, or 0 with
 * the first problem. Hosts may run it on cores they did not build. */
int nacre_core_validate(const NacreCore *core, char *error, size_t error_capacity);

/* The built-in tables. */
extern const NacreCore nacre_chip8_core;
extern const NacreCore nacre_gb_core;

#ifdef __cplusplus
}
#endif

#endif
