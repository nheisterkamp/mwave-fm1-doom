//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
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
//	DOOM main program (D_DoomMain) and game loop (D_DoomLoop),
//	plus functions to determine game mode (shareware, registered),
//	parse command line parameters, configure game parameters (turbo),
//	and call the startup functions.
//


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "deh_main.h"
#include "doomdef.h"
#include "doomstat.h"

#include "dstrings.h"
#include "doomfeatures.h"
#include "sounds.h"


#include "z_zone.h"
#include "w_wad.h"
#include "s_sound.h"
#include "v_video.h"

#include "f_finale.h"

#include "m_argv.h"
#include "m_config.h"
#include "m_controls.h"
#include "m_misc.h"
#include "m_menu.h"
#include "p_saveg.h"

#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"

#include "g_game.h"

#include "hu_stuff.h"
#include "wi_stuff.h"
#include "st_stuff.h"
#include "am_map.h"

#include "p_setup.h"
#include "r_local.h"

#include "d_main.h"
#include "doomgeneric.h"

//
// D-DoomLoop()
// Not a globally visible function,
//  just included for source reference,
//  called by D_DoomMain, never exits.
// Manages timing and IO,
//  calls all ?_Responder, ?_Ticker, and ?_Drawer,
//  calls I_GetTime, I_StartFrame, and I_StartTic
//
void D_DoomLoop (void);

// Location where savegames are stored

char *          savegamedir;

// location of IWAD and WAD files

char *          iwadfile;


boolean		devparm;	// started game with -devparm
boolean         nomonsters;	// checkparm of -nomonsters
boolean         respawnparm;	// checkparm of -respawn
boolean         fastparm;	// checkparm of -fast

//extern int soundVolume;
//extern  int	sfxVolume;
//extern  int	musicVolume;

extern  boolean	inhelpscreens;

skill_t		startskill;
int             startepisode;
int		startmap;
boolean		autostart;
int             startloadgame;

boolean		advancedemo;

// Store demo, do not accept any inputs
boolean         storedemo;

// "BFG Edition" version of doom2.wad does not include TITLEPIC.
boolean         bfgedition;

// If true, the main game loop has started.
boolean         main_loop_started = false;


int             show_endoom = 1;


void D_ConnectNetGame(void);
void D_CheckNetGame(void);


//
// D_ProcessEvents
// Send all the events of the given timestamp down the responder chain
//
void D_ProcessEvents (void)
{
    event_t*	ev;
	
    // IF STORE DEMO, DO NOT ACCEPT INPUT
    if (storedemo)
        return;
	
    while ((ev = D_PopEvent()) != NULL)
    {
	if (M_Responder (ev))
	    continue;               // menu ate the event
	G_Responder (ev);
    }
}




//
// D_Display
//  draw current display, possibly wiping it from the previous
//

// wipegamestate can be set to -1 to force a wipe on the next draw
gamestate_t     wipegamestate = GS_DEMOSCREEN;
extern  boolean setsizeneeded;
extern  int             showMessages;
void R_ExecuteSetViewSize (void);

void D_Display (void)
{
    static  boolean		viewactivestate = false;
    static  boolean		menuactivestate = false;
    static  boolean		inhelpscreensstate = false;
    static  boolean		fullscreen = false;
    static  gamestate_t		oldgamestate = -1;
    static  int			borderdrawcount;
    int				y;
    boolean			wipe;
    boolean			redrawsbar;

    if (nodrawers)
    	return;                    // for comparative timing / profiling
		
    redrawsbar = false;
    
    // change the view size if needed
    if (setsizeneeded)
    {
		R_ExecuteSetViewSize ();
		oldgamestate = -1;                      // force background redraw
		borderdrawcount = 3;
    }

    // no screen wipes on the FM-1 (their two extra screens do not fit):
    // a change of state only redraws the status bar
    wipe = gamestate != wipegamestate;

    if (gamestate == GS_LEVEL && gametic)
    	HU_Erase();
    
    // do buffered drawing
    switch (gamestate)
    {
      case GS_LEVEL:
		if (!gametic)
			break;
		if (automapactive)
			AM_Drawer ();
		if (wipe || (viewheight != 200 && fullscreen) )
			redrawsbar = true;
		if (inhelpscreensstate && !inhelpscreens)
			redrawsbar = true;              // just put away the help screen
		ST_Drawer (viewheight == 200, redrawsbar );
		fullscreen = viewheight == 200;
		break;

      case GS_INTERMISSION:
		WI_Drawer ();
		break;

      case GS_FINALE:
		F_Drawer ();
		break;

      case GS_DEMOSCREEN:
		D_PageDrawer ();
		break;
    }
    
    // draw buffered stuff to screen
    I_UpdateNoBlit ();
    
    // draw the view directly
    if (gamestate == GS_LEVEL && !automapactive && gametic)
    	R_RenderPlayerView (&players[displayplayer]);

    if (gamestate == GS_LEVEL && gametic)
    	HU_Drawer ();
    
    // clean up border stuff
    if (gamestate != oldgamestate && gamestate != GS_LEVEL)
    	I_SetPalette (W_CacheLumpName (DEH_String("PLAYPAL"),PU_CACHE));

    // see if the border needs to be initially drawn
    if (gamestate == GS_LEVEL && oldgamestate != GS_LEVEL)
    {
		viewactivestate = false;        // view was not active
		R_FillBackScreen ();    // draw the pattern into the back screen
    }

    // see if the border needs to be updated to the screen
    if (gamestate == GS_LEVEL && !automapactive && scaledviewwidth != 320)
    {
		if (menuactive || menuactivestate || !viewactivestate)
			borderdrawcount = 3;
		if (borderdrawcount)
		{
			R_DrawViewBorder ();    // erase old menu stuff
			borderdrawcount--;
		}
    }

    if (testcontrols)
    {
        // Box showing current mouse speed

        V_DrawMouseSpeedBox(testcontrols_mousespeed);
    }

    menuactivestate = menuactive;
    viewactivestate = viewactive;
    inhelpscreensstate = inhelpscreens;
    oldgamestate = wipegamestate = gamestate;
    
    // draw pause pic
    if (paused)
    {
		if (automapactive)
			y = 4;
		else
			y = viewwindowy+4;
		V_DrawPatchDirect(viewwindowx + (scaledviewwidth - 68) / 2, y,
							  W_CacheLumpName (DEH_String("M_PAUSE"), PU_CACHE));
    }


    // menus go directly to the screen
    M_Drawer ();          // menu is drawn even on top of everything
    NetUpdate ();         // send out any new accumulation


    I_FinishUpdate ();              // page flip or blit buffer
}

//
// Add configuration file variable bindings.
//


//
// D_GrabMouseCallback
//
// Called to determine whether to grab the mouse pointer
//


void doomgeneric_Tick()
{
    // frame syncronous IO operations
    I_StartFrame ();

    TryRunTics (); // will run at least one tic

    S_UpdateSounds (players[consoleplayer].mo);// move positional sounds

    // Update display, next frame, with current state.
    if (screenvisible)
    {
        D_Display ();
    }
}

//
//  D_DoomLoop
//
void D_DoomLoop (void)
{
    if (bfgedition &&
        (demorecording || (gameaction == ga_playdemo) || netgame))
    {
        printf(" WARNING: You are playing using one of the Doom Classic\n"
               " IWAD files shipped with the Doom 3: BFG Edition. These are\n"
               " known to be incompatible with the regular IWAD files and\n"
               " may cause demos and network games to get out of sync.\n");
    }

    if (demorecording)
    	G_BeginRecording ();

    main_loop_started = true;

    TryRunTics();

    I_SetWindowTitle(gamedescription);
    I_GraphicsCheckCommandLine();
    I_InitGraphics();
    I_EnableLoadingDisk();

    V_RestoreBuffer();
    R_ExecuteSetViewSize();

    D_StartGameLoop();

    if (testcontrols)
    {
        wipegamestate = gamestate;
    }

    doomgeneric_Tick();
}



//
//  DEMO LOOP
//
int             demosequence;
int             pagetic;
char                    *pagename;


//
// D_PageTicker
// Handles timing for warped projection
//
void D_PageTicker (void)
{
    if (--pagetic < 0)
	D_AdvanceDemo ();
}



//
// D_PageDrawer
// A page the image leaves out (the title, with no room for it): the menu's
// DOOM logo where the menu has it, on black, and the way into the menu.
//
void D_PageDrawer (void)
{
    if (W_CheckNumForName(pagename) >= 0)
    {
        V_DrawPatch (0, 0, W_CacheLumpName(pagename, PU_CACHE));
        return;
    }
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
    if (menuactive)
        return;                 // (the menus draw their own)
    if (W_CheckNumForName("M_DOOM") >= 0)
        V_DrawPatch (94, 2, W_CacheLumpName("M_DOOM", PU_CACHE));
    M_WriteText(160 - M_StringWidth("FM-1 EDITION") / 2, 140, "FM-1 EDITION");
    M_WriteText(160 - M_StringWidth("PRESS HOME") / 2, 170, "PRESS HOME");
}


//
// D_AdvanceDemo
// Called after each demo or intro demosequence finishes
//
void D_AdvanceDemo (void)
{
    advancedemo = true;
}

// Whether a step of the demo sequence has what it shows
static boolean D_DemoStepAvailable(int step)
{
    static char *const lumps[7] = {
        NULL, "demo1", "CREDIT", "demo2", "HELP2", "demo3", "demo4" };

    if (step == 4 && gamemode == commercial)
        return true;
    if (step == 4 && gamemode == retail)
        return W_CheckNumForName("CREDIT") >= 0;
    return lumps[step] == NULL || W_CheckNumForName(lumps[step]) >= 0;
}

//
// This cycles through the demo sequences.
// The image may leave out the demos and pages (their maps are not in it):
// those steps are skipped, at the worst the title stays up.
//
void D_DoAdvanceDemo (void)
{
    int tries, steps;

    players[consoleplayer].playerstate = PST_LIVE;  // not reborn
    advancedemo = false;
    usergame = false;               // no save / end game here
    paused = false;
    gameaction = ga_nothing;

    // The Ultimate Doom executable changed the demo sequence to add
    // a DEMO4 demo.

    if (gameversion == exe_ultimate || gameversion == exe_final)
      steps = 7;
    else
      steps = 6;

    for (tries = 0; tries < steps; tries++)
    {
        demosequence = (demosequence+1)%steps;
        if (D_DemoStepAvailable(demosequence))
            break;
    }

    switch (demosequence)
    {
      case 0:
	if ( gamemode == commercial )
	    pagetic = TICRATE * 11;
	else
	    pagetic = 170;
	gamestate = GS_DEMOSCREEN;
	pagename = DEH_String("TITLEPIC");
	if ( gamemode == commercial )
	  S_StartMusic(mus_dm2ttl);
	else
	  S_StartMusic (mus_intro);
	break;
      case 1:
	G_DeferedPlayDemo(DEH_String("demo1"));
	break;
      case 2:
	pagetic = 200;
	gamestate = GS_DEMOSCREEN;
	pagename = DEH_String("CREDIT");
	break;
      case 3:
	G_DeferedPlayDemo(DEH_String("demo2"));
	break;
      case 4:
	gamestate = GS_DEMOSCREEN;
	if ( gamemode == commercial)
	{
	    pagetic = TICRATE * 11;
	    pagename = DEH_String("TITLEPIC");
	    S_StartMusic(mus_dm2ttl);
	}
	else
	{
	    pagetic = 200;

	    if ( gamemode == retail )
	      pagename = DEH_String("CREDIT");
	    else
	      pagename = DEH_String("HELP2");
	}
	break;
      case 5:
	G_DeferedPlayDemo(DEH_String("demo3"));
	break;
        // THE DEFINITIVE DOOM Special Edition demo
      case 6:
	G_DeferedPlayDemo(DEH_String("demo4"));
	break;
    }
}



//
// D_StartTitle
//
void D_StartTitle (void)
{
    gameaction = ga_nothing;
    demosequence = -1;
    D_AdvanceDemo ();
}

void D_IdentifyVersion(void)
{
    // the image holds Doom 1 maps (E1M1..) or Doom 2 maps (MAP01..)
    if (W_CheckNumForName("MAP01") >= 0)
    {
        gamemission = doom2;
        gamemode = commercial;
    }
    else
    {
        gamemission = doom;
        if (W_CheckNumForName("E4M1") > 0)
            gamemode = retail;
        else if (W_CheckNumForName("E3M1") > 0)
            gamemode = registered;
        else
            gamemode = shareware;
    }
}

void D_SetGameDescription(void)
{
    if (W_CheckNumForName("FREEDOOM") >= 0)
        gamedescription = logical_gamemission == doom ? "Freedoom: Phase 1" : "Freedoom: Phase 2";
    else if (logical_gamemission != doom)
        gamedescription = "DOOM 2: Hell on Earth";
    else if (gamemode == retail)
        gamedescription = "The Ultimate DOOM";
    else if (gamemode == registered)
        gamedescription = "DOOM Registered";
    else
        gamedescription = "DOOM Shareware";
}

static void InitGameVersion(void)
{
    if (gamemode == retail)
        gameversion = exe_ultimate;
    else
        gameversion = exe_doom_1_9;
}

//
// D_DoomMain
//
// Command line (the host build only; the FM-1 starts without one):
//   -warp E M, -skill N, -nomonsters, -respawn, -fast, -devparm,
//   -playdemo LUMP, -timedemo LUMP
//
void D_DoomMain (void)
{
    int p, err;
    char demolumpname[9];

    I_PrintBanner(PACKAGE_STRING);

    Z_Init ();

    nomonsters = M_CheckParm ("-nomonsters");
    respawnparm = M_CheckParm ("-respawn");
    fastparm = M_CheckParm ("-fast");
    devparm = M_CheckParm ("-devparm");

    V_Init ();

    err = W_InitPack(DG_PackPart(0), DG_PackPart(1), DG_PackPart(2));
    if (err)
    {
        I_Error("No game data in flash (DPK %d).\n"
                "Run tools/fm1_doom.py upload.", err);
    }
    W_GenerateHashTable();

    modifiedgame = false;
    D_IdentifyVersion();
    InitGameVersion();
    D_SetGameDescription();
    savegamedir = "";
    I_PrintStartupBanner(gamedescription);

    I_InitTimer();
    I_InitSound(true);
    I_InitMusic();

    D_ConnectNetGame();

    startskill = sk_medium;
    startepisode = 1;
    startmap = 1;
    autostart = false;

    p = M_CheckParmWithArgs("-skill", 1);
    if (p)
    {
	startskill = myargv[p+1][0]-'1';
	autostart = true;
    }

    p = M_CheckParmWithArgs("-warp", 1);
    if (p)
    {
        if (gamemode == commercial)
            startmap = atoi (myargv[p+1]);
        else
        {
            startepisode = myargv[p+1][0]-'0';
            if (p + 2 < myargc)
            {
                startmap = myargv[p+2][0]-'0';
            }
            else
            {
                startmap = 1;
            }
        }
        autostart = true;
    }

    timelimit = 0;
    startloadgame = -1;

    M_Init ();
    R_Init ();
    P_Init ();
    S_Init (sfxVolume * 8, musicVolume * 8);
    D_CheckNetGame ();
    HU_Init ();
    ST_Init ();

    p = M_CheckParmWithArgs("-playdemo", 1);
    if (p)
    {
        M_StringCopy(demolumpname, myargv[p + 1], sizeof(demolumpname));
        singledemo = true;              // quit after one demo
        G_DeferedPlayDemo (demolumpname);
        D_DoomLoop ();
        return;
    }

    p = M_CheckParmWithArgs("-timedemo", 1);
    if (p)
    {
        M_StringCopy(demolumpname, myargv[p + 1], sizeof(demolumpname));
        G_TimeDemo (demolumpname);
        D_DoomLoop ();
        return;
    }

    if (autostart)
        G_InitNew (startskill, startepisode, startmap);
    else
        D_StartTitle ();                // start up intro loop

    D_DoomLoop ();
}
