// SPDX-License-Identifier: GPL-3.0-only
// The FM-1's keys, buttons and knobs -> Doom's input events. Portable: the firmware feeds it from the key
// scan, the host build from a script.
#pragma once
#include "d_event.h"

/* controls: the 14 buttons by matrix id (hal/fm1_input.h), then the 27 note keys F3..G5 */
enum {
    FM1C_OCTDN, FM1C_OCTUP, FM1C_FX, FM1C_SCL, FM1C_ENV, FM1C_LFO, FM1C_EDIT, FM1C_GLO, FM1C_HOME, FM1C_SAVE,
    FM1C_ARP, FM1C_SEQ, FM1C_PLAY, FM1C_REC,
    FM1C_KEY0,                                   /* F3; FM1C_KEY0 + n = note key n */
    FM1C_COUNT = FM1C_KEY0 + 27
};
/* knobs (encoders) by matrix id */
enum { FM1E_SELECT = 0, FM1E_ALGO = 1, FM1E_K1 = 2, FM1E_K2 = 3, FM1E_K3 = 4, FM1E_K4 = 5, FM1E_PRESETS = 6 };

void fm1_map_control(int control, int down);    /* a button or key went down / up */
void fm1_map_encoder(int enc, int steps);        /* a knob turned, + = clockwise */
int fm1_map_event(event_t *ev);                  /* the next event for the engine, 0 when none */
int fm1_control_by_name(const char *name);       /* "PLAY", "OCT-", "F3", "G#3" ... -> control, -1 */
int fm1_control_used(int control);               /* does it do anything in Doom (its LED glows) */
