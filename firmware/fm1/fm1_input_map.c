// SPDX-License-Identifier: GPL-3.0-only
/* The controls of Doom on the FM-1 (README "Controls"):
 *
 *   keys, left hand    F3 strafe left   G3 turn left   A3 back   B3 turn right   C4 strafe right
 *                      F#3 G#3 A#3 forward   C#4 run (held)
 *   keys, right hand   C5 fire   D5 use   E5 fire   B4 run (held)   C#5 previous weapon   D#5 next weapon
 *   buttons            FX SCL ENV LFO EDIT GLO: weapons 1..6   ARP: weapon 7
 *                      PLAY fire   REC use   HOME menu   SEQ map   OCT- / OCT+ previous / next weapon
 *   knobs              SELECT, KNOB 1..4: turn
 *
 * In the menus the movement keys move the cursor and fire / use pick (m_menu.c takes KEY_FIRE and
 * KEY_USE as "enter" and "yes"). Several controls can hold one Doom key: it goes up when the last lets go. */
#include <string.h>

#include "doomkeys.h"
#include "fm1_input_map.h"

#define KEY_PREVWEAPON 0x5b      /* '[': m_controls.c key_prevweapon */
#define KEY_NEXTWEAPON 0x5d      /* ']': key_nextweapon */
#define TURN_PER_STEP 90         /* mouse units per knob click: 90 x 8 / 65536 of a turn, ~4 degrees */

/* Doom key of each control, 0 = none */
static const unsigned char CONTROL_KEY[FM1C_COUNT] = {
    [FM1C_OCTDN] = KEY_PREVWEAPON, [FM1C_OCTUP] = KEY_NEXTWEAPON,
    [FM1C_FX] = '1', [FM1C_SCL] = '2', [FM1C_ENV] = '3', [FM1C_LFO] = '4', [FM1C_EDIT] = '5',
    [FM1C_GLO] = '6', [FM1C_ARP] = '7',
    [FM1C_HOME] = KEY_ESCAPE, [FM1C_SEQ] = KEY_TAB, [FM1C_PLAY] = KEY_FIRE, [FM1C_REC] = KEY_USE,
    [FM1C_SAVE] = 0,
    /* left hand: F3 F#3 G3 G#3 A3 A#3 B3 C4 C#4 */
    [FM1C_KEY0 + 0] = KEY_STRAFE_L, [FM1C_KEY0 + 1] = KEY_UPARROW, [FM1C_KEY0 + 2] = KEY_LEFTARROW,
    [FM1C_KEY0 + 3] = KEY_UPARROW, [FM1C_KEY0 + 4] = KEY_DOWNARROW, [FM1C_KEY0 + 5] = KEY_UPARROW,
    [FM1C_KEY0 + 6] = KEY_RIGHTARROW, [FM1C_KEY0 + 7] = KEY_STRAFE_R, [FM1C_KEY0 + 8] = KEY_RSHIFT,
    /* right hand: B4 C5 C#5 D5 D#5 E5 */
    [FM1C_KEY0 + 18] = KEY_RSHIFT, [FM1C_KEY0 + 19] = KEY_FIRE, [FM1C_KEY0 + 20] = KEY_PREVWEAPON,
    [FM1C_KEY0 + 21] = KEY_USE, [FM1C_KEY0 + 22] = KEY_NEXTWEAPON, [FM1C_KEY0 + 23] = KEY_FIRE,
};

static unsigned char held[256];                  /* controls holding each Doom key */
static unsigned char control_down[FM1C_COUNT];

#define QN 32u
static event_t queue[QN];
static unsigned qr, qw;

static void post(evtype_t type, int data1, int data2)
{
    event_t *e;
    if (qw - qr >= QN)
        return;
    e = &queue[qw % QN];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->data1 = data1;
    e->data2 = data2;
    qw++;
}

void fm1_map_control(int control, int down)
{
    unsigned char k;
    if (control < 0 || control >= FM1C_COUNT || control_down[control] == (unsigned char)!!down)
        return;
    control_down[control] = (unsigned char)!!down;
    k = CONTROL_KEY[control];
    if (!k)
        return;
    if (down) {
        if (held[k]++ == 0)
            post(ev_keydown, k, k);
    } else if (held[k] && --held[k] == 0)
        post(ev_keyup, k, k);
}

void fm1_map_encoder(int enc, int steps)
{
    if (!steps)
        return;
    switch (enc) {
    case FM1E_SELECT: case FM1E_K1: case FM1E_K2: case FM1E_K3: case FM1E_K4:
        post(ev_mouse, 0, steps * TURN_PER_STEP);
        break;
    default:
        break;
    }
}

int fm1_control_used(int control)
{
    return control >= 0 && control < FM1C_COUNT && CONTROL_KEY[control] != 0;
}

int fm1_map_event(event_t *ev)
{
    if (qr == qw)
        return 0;
    *ev = queue[qr % QN];
    qr++;
    return 1;
}

int fm1_control_by_name(const char *name)
{
    static const char *const BUTTONS[14] = {"OCT-", "OCT+", "FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "HOME",
                                            "SAVE", "ARP", "SEQ", "PLAY", "REC"};
    static const char *const NOTES[12] = {"F", "F#", "G", "G#", "A", "A#", "B", "C", "C#", "D", "D#", "E"};
    int i, n;
    for (i = 0; i < 14; i++)
        if (!strcmp(name, BUTTONS[i]))
            return i;
    for (n = 0; n < 27; n++) {                   /* note key n: F3 + n semitones */
        int pc = n % 12, oct = 3 + (n + 5) / 12;  /* F3..E4 octave 3, F4.. but C is the octave change */
        size_t l = strlen(NOTES[pc]);
        if (!strncmp(name, NOTES[pc], l) && name[l] == (char)('0' + oct) && !name[l + 1])
            return FM1C_KEY0 + n;
    }
    return -1;
}
