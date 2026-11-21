#ifndef NACRE_PAD_H
#define NACRE_PAD_H

/* Gamepad input through SDL3, shared by the Windows, Mac and Linux frontends.
 * Any recognised controller works; up to four are read at once and their inputs
 * are merged. For a core with only A and B (the Game Boy):
 *
 *   D-pad or left stick   D-pad
 *   East / North          A          South / West          B
 *   Start                 Start      Back                  Select
 *   Right shoulder or either trigger   fast-forward (hold)
 *   Left shoulder (hold) + Start       save state
 *   Left shoulder (hold) + Back        load state
 *   Left shoulder (hold) + North       next save slot
 *
 * A core that labels North, West and the shoulders (the DS) gets them as X, Y, L
 * and R; then the right trigger fast-forwards and the left trigger is the hotkey
 * modifier. Face buttons are named by position, so the layout matches the console
 * on any pad. */

#include "gb_input.h"

/* A bit per GbInput action that a controller is currently holding. */
#define PAD_BIT(action) (1u << (action))

void pad_init(void);
void pad_shutdown(void);

/* Reads the controllers and returns the held actions, mapped for a core that labels
 * `labeled` (see gbi_labeled_buttons). Cheap enough to call every frame. */
unsigned pad_poll(uint32_t labeled);

/* A one-time note such as "Controller connected: ..." once something is plugged in
 * or removed, then NULL until the next change. */
const char *pad_take_message(void);

#endif
