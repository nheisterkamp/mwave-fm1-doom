// SPDX-License-Identifier: GPL-2.0-or-later

#include "m_argv.h"

#include "doomgeneric.h"

void D_DoomMain (void);

void doomgeneric_Create(int argc, char **argv)
{
    myargc = argc;
    myargv = argv;

    DG_Init();

    D_DoomMain ();
}
