/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2006 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  DOOM graphics stuff for SDL
 *
 *-----------------------------------------------------------------------------
 */

#include "config.h"
#include <stdlib.h>
#include <unistd.h>
#include "m_argv.h"
#include "doomstat.h"
#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "r_draw.h"
#include "d_main.h"
#include "d_event.h"
#include "i_video.h"
#include "z_zone.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "lprintf.h"

#include "esp_task.h"
#include "esp_heap_caps.h"
#include "frame_queue.h"

int use_doublebuffer = 0;
int use_fullscreen = 0;
int desired_fullscreen = 0;

extern frame_queue_t g_frame_queue;

static uint8_t *next_frame_buffer = NULL;
static uint8_t current_palette = 0;

/* I_StartTic
 * Called by D_DoomLoop,
 * called before processing each tic in a frame.
 * Quick synchronous operations are performed here.
 * Can call D_PostEvent.
 */
void I_StartTic (void)
{
}

void I_ShutdownGraphics(void)
{
}

//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
}

/* I_StartFrame
 * Called by D_DoomLoop,
 * called before processing any tics in a frame
 * (just after displaying a frame).
 * Time consuming synchronous operations
 * are performed here (joystick reading).
 * Can call D_PostEvent.
 */
void I_StartFrame (void)
{
  next_frame_buffer = frame_queue_get_write_buffer(&g_frame_queue);
  if (!next_frame_buffer) {
    // Queue is full, need to wait for the next available buffer
    //TODO: Handle this case
  }
  screens[0].data = next_frame_buffer + 1; // Skip the palette index at position 0
}


int I_StartDisplay(void)
{
  return 1;
}

void I_EndDisplay(void)
{
}

//
// I_FinishUpdate
//

void I_FinishUpdate (void)
{  
  if (next_frame_buffer) {
    // Set palette index at position 0
    next_frame_buffer[0] = current_palette;
    frame_queue_submit_frame(&g_frame_queue);
  }
}

void I_SetPalette (int pal)
{
	current_palette = pal; // Update current palette index
	int pplump = W_GetNumForName("PLAYPAL");
	const byte * palette = W_CacheLumpNum(pplump);
	palette+=pal*(3*256);
  for (int i=0; i<255 ; i++) {
    int v=((palette[0]>>3)<<11)+((palette[1]>>2)<<5)+(palette[2]>>3);
		palette += 3;
	}
	W_UnlockLumpNum(pplump);
}

void I_PreInitGraphics(void)
{
	lprintf(LO_INFO, "preinitgfx");
	// Allocate screen buffer in PSRAM since it's large (320x240 = 76.8KB)
	// Internal memory is limited and we need it for other allocations
	screenbuf = heap_caps_malloc(SCREENWIDTH*SCREENHEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	assert(screenbuf);
	
	lprintf(LO_INFO, "Allocated framebuffers: main=%p, size=%d", 
	        screenbuf, SCREENWIDTH*SCREENHEIGHT);
}


// CPhipps -
// I_SetRes
// Sets the screen resolution
void I_SetRes(void)
{
  // set first three to standard values
  for (int i=0; i<3; i++) {
    screens[i].width = SCREENWIDTH;
    screens[i].height = SCREENHEIGHT;
    screens[i].byte_pitch = SCREENPITCH;
    screens[i].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
    screens[i].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);
  }

  // statusbar
  screens[4].width = SCREENWIDTH;
  screens[4].height = (ST_SCALED_HEIGHT+1);
  screens[4].byte_pitch = SCREENPITCH;
  screens[4].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
  screens[4].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);

  screens[0].not_on_heap=true;
  screens[0].data=screenbuf;
  assert(screens[0].data);

  lprintf(LO_INFO,"I_SetRes: Using resolution %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
}

void I_InitGraphics(void)
{
  static int    firsttime=1;

  if (firsttime)
  {
    firsttime = 0;

    atexit(I_ShutdownGraphics);
    lprintf(LO_INFO, "I_InitGraphics: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);

    /* Set the video mode */
    I_UpdateVideoMode();
  }
}


void I_UpdateVideoMode(void)
{
  video_mode_t mode;

  lprintf(LO_INFO, "I_UpdateVideoMode: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
  mode = VID_MODE8;

  V_InitMode(mode);
  V_DestroyUnusedTrueColorPalettes();
  V_FreeScreens();

  I_SetRes();

  V_AllocScreens();

  R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);

}
