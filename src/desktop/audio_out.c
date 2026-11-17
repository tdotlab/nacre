/* One audio stream for every core; see audio_out.h. */

#include "audio_out.h"

#include <SDL3/SDL.h>

#include <stdio.h>

#define CHUNK_FRAMES 4096
#define QUEUE_LOW_MS 20.0f    /* below this, pad with silence ... */
#define QUEUE_TARGET_MS 40.0f /* ... up to this */
#define QUEUE_HIGH_MS 150.0f  /* above this, drop the next batch */

struct AudioOut {
    SDL_AudioStream *stream;
    unsigned rate;        /* rate the stream was opened at; 0 when there is none */
    unsigned failed_rate; /* a rate no device took, so it isn't retried every frame */
    const void *owner;    /* the core instance whose sound is queued */
    int running;
    int16_t samples[CHUNK_FRAMES * 2];
};

static const int16_t silence[480 * 2] = { 0 };

AudioOut *audio_out_create(void) {
    return (AudioOut *)SDL_calloc(1, sizeof(AudioOut));
}

static void close_stream(AudioOut *out) {
    if (out->stream) SDL_DestroyAudioStream(out->stream);
    out->stream = NULL;
    out->rate = 0;
    out->running = 0;
}

void audio_out_destroy(AudioOut *out) {
    if (!out) return;
    close_stream(out);
    SDL_free(out);
}

int audio_out_available(const AudioOut *out) { return out && out->stream != NULL; }

/* Streams open paused. */
static void open_stream(AudioOut *out, unsigned rate) {
    SDL_AudioSpec spec;
    close_stream(out);
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = (int)rate;
    out->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (out->stream) {
        out->rate = rate;
        out->failed_rate = 0;
    } else {
        out->failed_rate = rate;
        fprintf(stderr, "audio is unavailable; continuing silently: %s\n", SDL_GetError());
    }
}

static void set_running(AudioOut *out, int run) {
    if (run == out->running) return;
    if (out->stream) {
        SDL_ClearAudioStream(out->stream);
        if (run) SDL_ResumeAudioStreamDevice(out->stream);
        else SDL_PauseAudioStreamDevice(out->stream);
    }
    out->running = run;
}

void audio_out_pump(AudioOut *out, const NacreCore *core, void *self, unsigned speed_percent,
                    int playing, int silent) {
    if (!out) return;
    if (!core || !self) {
        set_running(out, 0);
        out->owner = NULL;
        return;
    }

    unsigned rate = core->audio_rate(self);
    if (rate && rate != out->rate && rate != out->failed_rate) open_stream(out, rate);
    if (self != out->owner) { /* a new game: nothing of the old one's sound survives */
        out->owner = self;
        if (out->stream) SDL_ClearAudioStream(out->stream);
    }

    int run = playing && out->stream != NULL;
    if (run && !out->running) core->audio_clear(self); /* don't replay what piled up while stopped */
    set_running(out, run);
    if (!run || silent) { /* fast-forward is a squeal, not music: let the queue drain */
        core->audio_clear(self);
        return;
    }

    float ratio = (float)(speed_percent ? speed_percent : 100u) / 100.0f;
    SDL_SetAudioStreamFrequencyRatio(out->stream, ratio);
    float bytes_per_ms = (float)out->rate * 4.0f / 1000.0f * ratio;
    int queued = SDL_GetAudioStreamQueued(out->stream);
    int drop = queued > (int)(QUEUE_HIGH_MS * bytes_per_ms);
    if (queued >= 0 && queued < (int)(QUEUE_LOW_MS * bytes_per_ms)) {
        int pad = (int)(QUEUE_TARGET_MS * bytes_per_ms) - queued;
        while (pad >= 4) {
            int chunk = pad < (int)sizeof(silence) ? (pad & ~3) : (int)sizeof(silence);
            SDL_PutAudioStreamData(out->stream, silence, chunk);
            pad -= chunk;
        }
    }
    size_t frames;
    while ((frames = core->audio_read(self, out->samples, CHUNK_FRAMES)) > 0) {
        if (!drop) SDL_PutAudioStreamData(out->stream, out->samples, (int)(frames * 4));
    }
}
