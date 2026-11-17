/* One running game behind the core interface; see nacre_session.h. */

#include "nacre_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPTIONS 16

typedef struct {
    char core[16];
    char key[32];
    char value[64];
} Option;

struct NacreSession {
    NacreSessionIo io;
    NacreHost host;
    int persist;

    const NacreCore *core;
    void *self;
    char *path;
    char *save_path; /* NULL when there is no battery save to keep */
    char save_note[320];
    int fault_reported;
    int save_pending;
    uint64_t save_due;

    int running;
    unsigned speed;
    Option options[MAX_OPTIONS];
    int option_count;
};

static char *copy_text(const char *text) {
    size_t n = strlen(text) + 1;
    char *out = (char *)malloc(n);
    if (out) memcpy(out, text, n);
    return out;
}

/* "Tetris.gb" + ".sav" -> "Tetris.sav", in the same folder. */
static char *sibling(const char *path, const char *extension) {
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    const char *dot = strrchr(path, '.');
    size_t stem, n;
    char *out;
    if (back && (!slash || back > slash)) slash = back;
    stem = (dot && (!slash || dot > slash + 1)) ? (size_t)(dot - path) : strlen(path);
    n = stem + strlen(extension) + 1;
    out = (char *)malloc(n);
    if (!out) return NULL;
    memcpy(out, path, stem);
    memcpy(out + stem, extension, strlen(extension) + 1);
    return out;
}

static const char *file_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

static int64_t now_unix(const NacreSession *session) {
    return session->io.now_unix ? session->io.now_unix(session->io.userdata) : 0;
}

static void set_clock(NacreSession *session) {
    if (session->core->set_wall_clock) session->core->set_wall_clock(session->self, now_unix(session));
}

NacreSession *nacre_session_create(const NacreSessionIo *io, const NacreHost *host, int persist) {
    NacreSession *session = (NacreSession *)calloc(1, sizeof(NacreSession));
    if (!session) return NULL;
    if (io) session->io = *io;
    if (host) session->host = *host;
    session->persist = persist != 0;
    session->running = 1;
    session->speed = 100;
    return session;
}

void nacre_session_flush(NacreSession *session) {
    const NacreCore *core = session->core;
    size_t size, written = 0;
    uint8_t *data;
    session->save_pending = 0;
    if (!session->self || !session->save_path || !session->persist || !session->io.write_file) return;
    if (!core->save_dirty(session->self)) return;
    set_clock(session);
    size = core->save_size(session->self);
    data = size ? (uint8_t *)malloc(size) : NULL;
    if (data) written = core->save_write(session->self, data, size);
    if (written && session->io.write_file(session->io.userdata, session->save_path, data, written)) {
        core->save_mark_clean(session->self);
        snprintf(session->save_note, sizeof session->save_note, "Saved to %s",
                 file_name(session->save_path));
    } else {
        snprintf(session->save_note, sizeof session->save_note, "Couldn't write %s",
                 file_name(session->save_path));
    }
    free(data);
}

void nacre_session_close(NacreSession *session) {
    if (!session->self) return;
    nacre_session_flush(session);
    session->core->destroy(session->self);
    session->core = NULL;
    session->self = NULL;
    free(session->path);
    free(session->save_path);
    session->path = NULL;
    session->save_path = NULL;
    session->save_note[0] = '\0';
    session->fault_reported = 0;
    session->save_pending = 0;
}

void nacre_session_destroy(NacreSession *session) {
    if (!session) return;
    nacre_session_close(session);
    free(session);
}

/* Finds the battery save beside the content and loads it. A file that doesn't
 * fit is left alone rather than overwritten. */
static void attach_save(NacreSession *session) {
    const NacreCore *core = session->core;
    uint8_t *data = NULL;
    size_t size = 0;
    NacreIoResult result;
    if (!session->path || !core->save_size || !core->save_size(session->self)) return;
    session->save_path = sibling(session->path, ".sav");
    if (!session->save_path) return;
    set_clock(session);
    result = session->io.read_file
        ? session->io.read_file(session->io.userdata, session->save_path, NACRE_MAX_SAVE_BYTES, &data, &size)
        : NACRE_IO_MISSING;
    if (result == NACRE_IO_MISSING) {
        snprintf(session->save_note, sizeof session->save_note,
                 session->persist ? "Saves to %s" : "Not saving %s this run", file_name(session->save_path));
    } else if (result == NACRE_IO_OK && core->save_load(session->self, data, size)) {
        core->save_mark_clean(session->self);
        snprintf(session->save_note, sizeof session->save_note, "Loaded %s",
                 file_name(session->save_path));
    } else {
        snprintf(session->save_note, sizeof session->save_note,
                 "Ignored %s; it doesn't fit this cartridge", file_name(session->save_path));
        free(session->save_path);
        session->save_path = NULL;
    }
    free(data);
}

static void apply_settings(NacreSession *session) {
    const NacreCore *core = session->core;
    for (int i = 0; i < session->option_count; i++) {
        const Option *option = &session->options[i];
        if (core->set_option && !strcmp(option->core, core->id))
            core->set_option(session->self, option->key, option->value);
    }
    core->set_speed(session->self, session->speed);
    core->set_running(session->self, session->running);
}

int nacre_session_open(NacreSession *session, const NacreCore *core, const char *path,
                       const uint8_t *data, size_t size, char *error, size_t error_capacity) {
    NacreHost host = session->host;
    NacreContent content;
    char *owned_path = NULL;
    void *self;

    if (!core) {
        size_t head = size < NACRE_PROBE_BYTES ? size : NACRE_PROBE_BYTES;
        core = nacre_core_detect(data, data ? head : 0, size, path);
        if (!core) {
            snprintf(error, error_capacity,
                     "Nacre doesn't recognize this file as a game for any system it runs.");
            return 0;
        }
    }
    if (!nacre_core_validate(core, error, error_capacity)) return 0;
    if ((core->flags & NACRE_CORE_NEEDS_PATH) && !path) {
        snprintf(error, error_capacity, "%s games have to be opened from a file.", core->name);
        return 0;
    }
    if (path && !(owned_path = copy_text(path))) {
        snprintf(error, error_capacity, "out of memory");
        return 0;
    }

    /* Only one instance of such a core can exist, so the old one goes first. */
    if ((core->flags & NACRE_CORE_SINGLETON) && session->core == core) nacre_session_close(session);

    error[0] = '\0';
    self = core->create(&host, error, error_capacity);
    if (!self) {
        if (!error[0]) snprintf(error, error_capacity, "%s couldn't start.", core->name);
        free(owned_path);
        return 0;
    }
    content.data = data;
    content.size = size;
    content.path = path;
    if (!core->load(self, &content, error, error_capacity)) {
        if (!error[0]) snprintf(error, error_capacity, "%s couldn't load this file.", core->name);
        core->destroy(self);
        free(owned_path);
        return 0;
    }

    nacre_session_close(session); /* writes the old game's save */
    session->core = core;
    session->self = self;
    session->path = owned_path;
    attach_save(session);
    apply_settings(session);
    return 1;
}

void nacre_session_restart(NacreSession *session) {
    if (!session->self) return;
    nacre_session_flush(session);
    session->core->reset(session->self);
    session->core->audio_clear(session->self);
    session->core->set_running(session->self, session->running);
    session->fault_reported = 0;
}

void nacre_session_set_running(NacreSession *session, int running) {
    session->running = running != 0;
    if (session->self) session->core->set_running(session->self, session->running);
}

void nacre_session_set_speed(NacreSession *session, unsigned percent) {
    session->speed = percent ? percent : 100;
    if (session->self) session->core->set_speed(session->self, session->speed);
}

int nacre_session_set_option(NacreSession *session, const char *core_id, const char *key,
                             const char *value) {
    Option *slot = NULL;
    if (!core_id || !key || !value || strlen(core_id) >= sizeof slot->core ||
        strlen(key) >= sizeof slot->key || strlen(value) >= sizeof slot->value)
        return 0;
    if (session->self && !strcmp(session->core->id, core_id)) {
        if (!session->core->set_option || !session->core->set_option(session->self, key, value))
            return 0;
    }
    for (int i = 0; i < session->option_count; i++)
        if (!strcmp(session->options[i].core, core_id) && !strcmp(session->options[i].key, key))
            slot = &session->options[i];
    if (!slot) {
        if (session->option_count == MAX_OPTIONS) return 0;
        slot = &session->options[session->option_count++];
        memcpy(slot->core, core_id, strlen(core_id) + 1); /* lengths checked above */
        memcpy(slot->key, key, strlen(key) + 1);
    }
    memcpy(slot->value, value, strlen(value) + 1);
    return 1;
}

int nacre_session_tick(NacreSession *session, uint64_t elapsed_ns, uint64_t now_ns) {
    const NacreCore *core = session->core;
    const char *fault;
    int ok = 1;
    if (!session->self) return 1;
    if (core->advance) core->advance(session->self, elapsed_ns);
    fault = core->error(session->self);
    if (fault && !session->fault_reported) {
        session->fault_reported = 1;
        nacre_session_flush(session); /* keep what the player had */
        ok = 0;
    } else if (!fault) {
        session->fault_reported = 0;
    }
    /* Games write their save RAM in bursts; wait for a quiet second. */
    if (session->save_path && session->persist && core->save_dirty(session->self)) {
        if (!session->save_pending) {
            session->save_pending = 1;
            session->save_due = now_ns + NACRE_SAVE_DELAY_NS;
        } else if (now_ns >= session->save_due) {
            nacre_session_flush(session);
        }
    } else {
        session->save_pending = 0;
    }
    return ok;
}

int nacre_session_has_states(const NacreSession *session) {
    return session->self && session->path && (session->core->flags & NACRE_CORE_HAS_STATES);
}

static char *state_file(const NacreSession *session, int slot) {
    char extension[16];
    if (!nacre_session_has_states(session) || slot < 0 || slot >= NACRE_STATE_SLOTS) return NULL;
    snprintf(extension, sizeof extension, ".state%d", slot + 1);
    return sibling(session->path, extension);
}

int nacre_session_state_path(const NacreSession *session, int slot, char *out, size_t capacity) {
    char *path = state_file(session, slot);
    int ok = path && strlen(path) < capacity;
    if (ok) memcpy(out, path, strlen(path) + 1);
    free(path);
    return ok;
}

int nacre_session_save_state(NacreSession *session, int slot, char *error, size_t error_capacity) {
    char *path = state_file(session, slot);
    uint8_t *data = NULL;
    size_t size = 0;
    int ok = 0;
    if (!path) {
        snprintf(error, error_capacity, "This game can't keep save states");
        return 0;
    }
    if (session->persist && session->io.write_file) {
        size = session->core->state_size(session->self);
        data = size && size <= NACRE_MAX_STATE_BYTES ? (uint8_t *)malloc(size) : NULL;
        if (data) size = session->core->state_save(session->self, data, size);
        ok = data && size && session->io.write_file(session->io.userdata, path, data, size);
    }
    if (!ok) snprintf(error, error_capacity, "Couldn't save to slot %d", slot + 1);
    free(data);
    free(path);
    return ok;
}

int nacre_session_load_state(NacreSession *session, int slot, char *error, size_t error_capacity) {
    char *path = state_file(session, slot);
    uint8_t *data = NULL;
    size_t size = 0;
    NacreIoResult result;
    int ok = 0;
    if (!path) {
        snprintf(error, error_capacity, "This game can't keep save states");
        return 0;
    }
    result = session->io.read_file
        ? session->io.read_file(session->io.userdata, path, NACRE_MAX_STATE_BYTES, &data, &size)
        : NACRE_IO_MISSING;
    if (result == NACRE_IO_MISSING) {
        snprintf(error, error_capacity, "Slot %d is empty", slot + 1);
    } else if (result != NACRE_IO_OK) {
        snprintf(error, error_capacity, "Couldn't read slot %d", slot + 1);
    } else if (!session->core->state_load(session->self, data, size)) {
        snprintf(error, error_capacity, "Slot %d doesn't fit this game", slot + 1);
    } else {
        session->core->audio_clear(session->self);
        session->fault_reported = 0;
        ok = 1;
    }
    free(data);
    free(path);
    return ok;
}

const NacreCore *nacre_session_core(const NacreSession *session) { return session->core; }

void *nacre_session_instance(const NacreSession *session) { return session->self; }

const char *nacre_session_path(const NacreSession *session) { return session->path; }

const char *nacre_session_error(const NacreSession *session) {
    return session->self ? session->core->error(session->self) : NULL;
}

const char *nacre_session_save_note(const NacreSession *session) { return session->save_note; }
