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
//	System interface: errors, exit, the zone's memory.
//

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "doomgeneric.h"
#include "m_misc.h"

#include "i_system.h"

#define MAX_ATEXIT 4

static atexit_func_t atexit_funcs[MAX_ATEXIT];
static boolean atexit_on_error[MAX_ATEXIT];
static int num_atexit;

void I_AtExit(atexit_func_t func, boolean run_on_error)
{
    if (num_atexit < MAX_ATEXIT)
    {
        atexit_funcs[num_atexit] = func;
        atexit_on_error[num_atexit] = run_on_error;
        num_atexit++;
    }
}

void I_Tactile(int on, int off, int total)
{
}

byte *I_ZoneBase (int *size)
{
    return DG_ZoneBase(size);
}

void I_PrintBanner(char *msg)
{
    DG_Log(msg);
}

void I_PrintDivider(void)
{
}

void I_PrintStartupBanner(char *gamedescription)
{
    DG_Log(gamedescription);
}

boolean I_ConsoleStdout(void)
{
    return false;
}

void I_Init (void)
{
}

void I_BindVariables(void)
{
}

void I_Quit (void)
{
    while (num_atexit > 0)
    {
        num_atexit--;
        atexit_funcs[num_atexit]();
    }
    DG_Quit();
}

void I_Error (char *error, ...)
{
    static boolean already_quitting = false;
    char msgbuf[160];
    va_list argptr;

    va_start(argptr, error);
    M_vsnprintf(msgbuf, sizeof(msgbuf), error, argptr);
    va_end(argptr);

    if (!already_quitting)
    {
        already_quitting = true;
        while (num_atexit > 0)
        {
            num_atexit--;
            if (atexit_on_error[num_atexit])
            {
                atexit_funcs[num_atexit]();
            }
        }
    }
    DG_Error(msgbuf);
}

//
// Read Access Violation emulation.
//
// From PrBoom+, by entryway.
//
// The null sector a broken map points at reads DOS's low memory; as vanilla
// under DOS 6.22.

#define DOS_MEM_DUMP_SIZE 10

static const unsigned char dos_mem_dump[DOS_MEM_DUMP_SIZE] = {
  0x57, 0x92, 0x19, 0x00, 0xF4, 0x06, 0x70, 0x00, 0x16, 0x00};

boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    switch (size)
    {
    case 1:
        *((unsigned char *) value) = dos_mem_dump[offset];
        return true;
    case 2:
        *((unsigned short *) value) = dos_mem_dump[offset]
                                    | (dos_mem_dump[offset + 1] << 8);
        return true;
    case 4:
        *((unsigned int *) value) = dos_mem_dump[offset]
                                  | (dos_mem_dump[offset + 1] << 8)
                                  | (dos_mem_dump[offset + 2] << 16)
                                  | (dos_mem_dump[offset + 3] << 24);
        return true;
    }

    return false;
}
