// SPDX-License-Identifier: GPL-2.0-or-later
//
// The platform under the engine. The FM-1 firmware implements it in
// firmware/fm1/plat.c, the host test build in host/host.c.

#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdint.h>

#include "d_event.h"

// Bring up the screen, keys and sound; called once before the engine starts
void DG_Init(void);

// A finished frame: SCREENWIDTH x SCREENHEIGHT palette indices
void DG_DrawFrame(const uint8_t *screen);

// The palette for the next frames: 256 x R, G, B (gamma applied)
void DG_SetPalette(const uint8_t *rgb);

uint32_t DG_GetTicksMs(void);
void DG_SleepMs(uint32_t ms);

// The next input event, 0 when there is none
int DG_GetEvent(event_t *ev);

// The zone: one block of RAM for the engine's allocator
uint8_t *DG_ZoneBase(int *size);

// The DPK image: part 0 in the firmware, parts 1 and 2 in the data areas
// (w_wad.h W_InitPack)
const void *DG_PackPart(int part);

// A line for the debug console
void DG_Log(const char *s);

// A fatal error: show it and restart. Never returns.
void DG_Error(const char *msg) __attribute__((noreturn));

// Quit from the menu. Never returns.
void DG_Quit(void) __attribute__((noreturn));

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

// Sound output: the platform pulls stereo frames at its own rate
// (the FM-1: from the audio interrupt). Safe against the engine's calls.
void I_MixSound(int16_t *stereo, int frames, int rate);

#endif //DOOM_GENERIC
