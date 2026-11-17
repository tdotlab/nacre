#ifndef NACRE_AUDIO_OUT_H
#define NACRE_AUDIO_OUT_H

/* Plays whatever core is running through one SDL3 audio stream. Cores hand over
 * stereo samples in step with emulated time; the device runs on its own crystal,
 * so the queue is held between about 40 and 150 ms: silence pads an underrun and a
 * long queue drops a batch. The stream follows the core's sample rate and the
 * session speed (a 2x game plays its sound 2x through the stream's frequency
 * ratio). Fast-forward runs silent.
 *
 * Shared by the SDL frontends. Call everything on the thread that owns the window. */

#include "nacre_core.h"

typedef struct AudioOut AudioOut;

AudioOut *audio_out_create(void);
void audio_out_destroy(AudioOut *out);

/* Once per frame, after the core ran. `core` and `self` may be NULL when nothing
 * is loaded. `playing` is 0 while paused or muted (the device stops and the queue
 * is dropped); `silent` drains the core without playing (fast-forward). */
void audio_out_pump(AudioOut *out, const NacreCore *core, void *self, unsigned speed_percent,
                    int playing, int silent);

/* Whether a device could be opened for the last core rate. */
int audio_out_available(const AudioOut *out);

#endif
