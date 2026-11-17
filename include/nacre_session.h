#ifndef NACRE_SESSION_H
#define NACRE_SESSION_H

/* One running game, whatever the system: picks the core, swaps it in only once
 * the new content has loaded, keeps the battery save beside the content file,
 * and manages save-state slots. Frontends own windows, devices and the clock;
 * file access goes through the I/O table so each platform keeps its own path
 * encoding and atomic writes. Paths here are UTF-8.
 *
 *   "Tetris.gb"  ->  "Tetris.sav", "Tetris.state1" .. "Tetris.state4"
 *
 * Plain C99; every call on the host's UI thread. */

#include "nacre_core.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NACRE_STATE_SLOTS 4
#define NACRE_MAX_SAVE_BYTES (1u << 20)
#define NACRE_MAX_STATE_BYTES (64u << 20)
/* Battery saves are written this long after the game last changed them. */
#define NACRE_SAVE_DELAY_NS 1000000000ull

typedef enum {
    NACRE_IO_OK = 0,
    NACRE_IO_MISSING = 1, /* no such file */
    NACRE_IO_FAILED = 2
} NacreIoResult;

typedef struct {
    /* Reads a whole file of at most `max_size` bytes into a buffer from malloc,
     * which the session frees. Larger files fail. */
    NacreIoResult (*read_file)(void *userdata, const char *path, size_t max_size, uint8_t **data,
                               size_t *size);
    /* Replaces a file without ever leaving a torn one (temp file, then rename).
     * Returns 1 on success. */
    int (*write_file)(void *userdata, const char *path, const uint8_t *data, size_t size);
    /* Unix seconds for real-time clocks. */
    int64_t (*now_unix)(void *userdata);
    void *userdata;
} NacreSessionIo;

typedef struct NacreSession NacreSession;

/* `host` (copied) is handed to every core instance the session creates. With
 * `persist` 0 nothing is ever written (self-checks, previews). */
NacreSession *nacre_session_create(const NacreSessionIo *io, const NacreHost *host, int persist);
/* Writes a pending battery save, then shuts the core down. */
void nacre_session_destroy(NacreSession *session);

/* Opens content. `path` may be NULL (built-in content; no saves or states then).
 * `core` forces a core (nacre_core_by_id, or one the host brings); NULL detects
 * one from the bytes and path. On failure the running game is kept and 0 is
 * returned with a message, except when a single-instance core had to be shut
 * down first: then the session is empty. The new game runs unless
 * set_running(0) is in force. */
int nacre_session_open(NacreSession *session, const NacreCore *core, const char *path,
                       const uint8_t *data, size_t size, char *error, size_t error_capacity);
/* Ends the game (writing its save) and leaves the session empty. */
void nacre_session_close(NacreSession *session);

/* Power cycle the current game; the battery save is written first. */
void nacre_session_restart(NacreSession *session);

/* Whether the user lets the game run (pause, menus, focus): applied now and to
 * every game opened later. */
void nacre_session_set_running(NacreSession *session, int running);
/* Speed in percent for this and later games (100 = real time). */
void nacre_session_set_speed(NacreSession *session, unsigned percent);
/* A core setting such as "cpu_hz" for "chip8". Remembered and re-applied each
 * time that core starts. Returns 1 if the current core took it, or if the core
 * isn't running now. */
int nacre_session_set_option(NacreSession *session, const char *core_id, const char *key,
                             const char *value);

/* Once per refresh. Runs stepped cores, notices faults in threaded ones, and
 * writes the battery save once it has been still for NACRE_SAVE_DELAY_NS.
 * `now_ns` is any monotonic clock. Returns 0 when a fault happened during this
 * call (see nacre_session_error); 1 otherwise. */
int nacre_session_tick(NacreSession *session, uint64_t elapsed_ns, uint64_t now_ns);

/* Writes the battery save now if it changed (before quitting, on focus loss). */
void nacre_session_flush(NacreSession *session);

/* Save states. Return 1, or 0 with a message ("Slot 2 is empty"). */
int nacre_session_save_state(NacreSession *session, int slot, char *error, size_t error_capacity);
int nacre_session_load_state(NacreSession *session, int slot, char *error, size_t error_capacity);
/* Whether states can be taken for the current game (a core that has them and a
 * content path to put them beside). */
int nacre_session_has_states(const NacreSession *session);
/* The slot's file path, or 0 if there is none. */
int nacre_session_state_path(const NacreSession *session, int slot, char *out, size_t capacity);

/* ---- What is running --------------------------------------------------- */

/* NULL when empty. The instance is for direct per-frame calls (video, input,
 * audio) through the core table. */
const NacreCore *nacre_session_core(const NacreSession *session);
void *nacre_session_instance(const NacreSession *session);
/* UTF-8 path of the open content, or NULL. */
const char *nacre_session_path(const NacreSession *session);
/* The current fault, or NULL. */
const char *nacre_session_error(const NacreSession *session);
/* One line about the battery save for a status area ("Loaded Tetris.sav",
 * "Couldn't write Tetris.sav"), or "" when the game has none. */
const char *nacre_session_save_note(const NacreSession *session);

#ifdef __cplusplus
}
#endif

#endif
