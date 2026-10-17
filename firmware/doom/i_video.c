//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2026 FM-1 Doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	The screen: the engine draws 8-bit pixels into I_VideoBuffer, the
//	platform turns each finished frame into the panel's pixels.
//

#include <string.h>

#include "doomtype.h"
#include "doomgeneric.h"
#include "d_event.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "z_zone.h"

#include "tables.h"

// The screen buffer; this is modified to draw things to the screen

byte *I_VideoBuffer = NULL;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible = true;

int usemouse = 1;
float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

// The palette as last set (gamma applied), for I_GetPaletteIndex

static byte cur_palette[256 * 3];

// The screen is a fixed buffer, not zone memory: it is allocated after the
// first level loads (D_DoomLoop), when 64 KB in one piece may not be left.
static byte video_buffer[SCREENWIDTH * SCREENHEIGHT] DOOM_POOL;

// The screen buffer as scratch space while nothing is drawn (w_wad.c
// W_LumpScratch: a level's map lumps unpacked one at a time)
byte *I_ScratchBuffer (int size)
{
    return size <= (int) sizeof(video_buffer) ? video_buffer : NULL;
}

void I_InitGraphics (void)
{
    I_VideoBuffer = video_buffer;
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
    screenvisible = true;
}

void I_ShutdownGraphics (void)
{
}

void I_StartFrame (void)
{
}

void I_UpdateNoBlit (void)
{
}

void I_FinishUpdate (void)
{
    DG_DrawFrame(I_VideoBuffer);
}

//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
void I_SetPalette (byte* palette)
{
    int i;

    for (i = 0; i < 256 * 3; i++)
    {
        cur_palette[i] = gammatable[usegamma][palette[i]];
    }
    DG_SetPalette(cur_palette);
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex (int r, int g, int b)
{
    int best, best_diff, diff;
    int i;

    best = 0;
    best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
        diff = (r - cur_palette[3 * i + 0]) * (r - cur_palette[3 * i + 0])
             + (g - cur_palette[3 * i + 1]) * (g - cur_palette[3 * i + 1])
             + (b - cur_palette[3 * i + 2]) * (b - cur_palette[3 * i + 2]);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

void I_BeginRead (void)
{
}

void I_EndRead (void)
{
}

void I_SetWindowTitle (char *title)
{
}

void I_GraphicsCheckCommandLine (void)
{
}

void I_SetGrabMouseCallback (grabmouse_callback_t func)
{
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables (void)
{
}

void I_DisplayFPSDots (boolean dots_on)
{
}

void I_CheckIsScreensaver (void)
{
}

//
// I_StartTic
// The input events since the last tic, into the engine's queue.
//
void I_StartTic (void)
{
    event_t ev;

    while (DG_GetEvent(&ev))
    {
        D_PostEvent(&ev);
    }
}
