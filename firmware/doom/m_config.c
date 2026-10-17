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
//    Configuration: the FM-1 has no files; the defaults are the
//    variables' initial values.
//

#include <stddef.h>

#include "doomtype.h"
#include "m_config.h"

char *configdir = "";

void M_LoadDefaults(void)
{
}

void M_SaveDefaults(void)
{
}

void M_SaveDefaultsAlternate(char *main, char *extra)
{
}

void M_SetConfigDir(char *dir)
{
}

void M_BindVariable(char *name, void *variable)
{
}

boolean M_SetVariable(char *name, char *value)
{
    return false;
}

int M_GetIntVariable(char *name)
{
    return 0;
}

const char *M_GetStrVariable(char *name)
{
    return NULL;
}

float M_GetFloatVariable(char *name)
{
    return 0;
}

void M_SetConfigFilenames(char *main_config, char *extra_config)
{
}

char *M_GetSaveGameDir(char *iwadname)
{
    return "";
}
