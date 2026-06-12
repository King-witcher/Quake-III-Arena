/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
//
/*
=======================================================================

DISPLAY OPTIONS MENU

=======================================================================
*/

#include "ui_local.h"


#define ART_FRAMEL			"menu/art/frame2_l"
#define ART_FRAMER			"menu/art/frame1_r"
#define ART_BACK0			"menu/art/back_0"
#define ART_BACK1			"menu/art/back_1"
#define ART_ACCEPT0			"menu/art/accept_0"
#define ART_ACCEPT1			"menu/art/accept_1"

#define ID_GRAPHICS			10
#define ID_DISPLAY			11
#define ID_SOUND			12
#define ID_NETWORK			13
#define ID_BRIGHTNESS		14
#define ID_SCREENSIZE		15
#define ID_BACK				16
#define ID_MODE				17
#define ID_FULLSCREEN		18
#define ID_RAYTRACING		19


typedef struct {
	menuframework_s	menu;

	menutext_s		banner;
	menubitmap_s	framel;
	menubitmap_s	framer;

	menutext_s		graphics;
	menutext_s		display;
	menutext_s		sound;
	menutext_s		network;
	menutext_s		raytracing;

	menuslider_s	brightness;
	menuslider_s	screensize;
	menulist_s		mode;
	menulist_s		fs;

	menubitmap_s	apply;
	menubitmap_s	back;

	int				initial_mode;
	int				initial_fs;
} displayOptionsInfo_t;

static displayOptionsInfo_t	displayOptionsInfo;

// Video Mode table — MUST mirror r_vidModes[] in the renderer (tr_init.c) one-to-one,
// since the array index is the r_mode value.  The Screen menu only shows the entries the
// desktop can actually display (from the r_availableModes cvar the renderer publishes), so
// e.g. a 1080p panel never lists the 4K / ultrawide modes.
typedef struct {
	const char	*label;
	int			width;
	int			height;
} uiVidMode_t;

static const uiVidMode_t ui_vidModes[] =
{
	{ "640x480 (4:3)",		640,	480  },	// 0
	{ "800x600 (4:3)",		800,	600  },	// 1
	{ "1024x768 (4:3)",		1024,	768  },	// 2
	{ "1280x960 (4:3)",		1280,	960  },	// 3
	{ "1600x1200 (4:3)",	1600,	1200 },	// 4
	{ "1280x1024 (5:4)",	1280,	1024 },	// 5
	{ "1280x720 (16:9)",	1280,	720  },	// 6
	{ "1366x768 (16:9)",	1366,	768  },	// 7
	{ "1600x900 (16:9)",	1600,	900  },	// 8
	{ "1920x1080 (16:9)",	1920,	1080 },	// 9
	{ "2560x1440 (16:9)",	2560,	1440 },	// 10
	{ "3840x2160 (16:9)",	3840,	2160 },	// 11
	{ "1280x800 (16:10)",	1280,	800  },	// 12
	{ "1680x1050 (16:10)",	1680,	1050 },	// 13
	{ "1920x1200 (16:10)",	1920,	1200 },	// 14
	{ "2560x1600 (16:10)",	2560,	1600 },	// 15
	{ "2560x1080 (21:9)",	2560,	1080 },	// 16
	{ "3440x1440 (21:9)",	3440,	1440 },	// 17
	{ "3840x1600 (21:9)",	3840,	1600 }	// 18
};
#define UI_NUM_VIDMODES ( (int)( sizeof( ui_vidModes ) / sizeof( ui_vidModes[0] ) ) )

// Filled by Screen_BuildModeList(): the visible (desktop-supported) subset.
static const char	*display_mode_names[UI_NUM_VIDMODES + 1];	// spinner labels (NULL-terminated)
static int			 display_mode_map[UI_NUM_VIDMODES];			// spinner index -> r_mode index
static int			 display_num_modes;

static const char *display_enabled_names[] =
{
	"Off",
	"On",
	0
};

/*
=================
Screen_ResIsAvailable

True if "WxH" appears as a whitespace-delimited token in the r_availableModes list.
=================
*/
static qboolean Screen_ResIsAvailable( const char *list, const char *token ) {
	const char	*p = list;
	int			tlen = strlen( token );

	while ( *p ) {
		while ( *p == ' ' ) {
			p++;
		}
		if ( !strncmp( p, token, tlen ) && ( p[tlen] == ' ' || p[tlen] == '\0' ) ) {
			return qtrue;
		}
		while ( *p && *p != ' ' ) {
			p++;
		}
	}
	return qfalse;
}

/*
=================
Screen_BuildModeList

Build the Video Mode spinner labels (and the spinner-index -> r_mode map) from the
desktop-supported resolutions the renderer published in r_availableModes.  Falls back to
the full table if the cvar is empty (e.g. the renderer has not created a window yet).
=================
*/
static void Screen_BuildModeList( void ) {
	char	avail[1024];
	char	token[32];
	int		i;

	trap_Cvar_VariableStringBuffer( "r_availableModes", avail, sizeof( avail ) );

	display_num_modes = 0;
	for ( i = 0; i < UI_NUM_VIDMODES; i++ ) {
		Com_sprintf( token, sizeof( token ), "%dx%d", ui_vidModes[i].width, ui_vidModes[i].height );
		if ( avail[0] && !Screen_ResIsAvailable( avail, token ) ) {
			continue;
		}
		display_mode_names[display_num_modes] = ui_vidModes[i].label;
		display_mode_map[display_num_modes]   = i;
		display_num_modes++;
	}

	if ( display_num_modes == 0 ) {
		// nothing matched (unexpected) — fall back to the whole table
		for ( i = 0; i < UI_NUM_VIDMODES; i++ ) {
			display_mode_names[i] = ui_vidModes[i].label;
			display_mode_map[i]   = i;
		}
		display_num_modes = UI_NUM_VIDMODES;
	}

	display_mode_names[display_num_modes] = NULL;
}

/*
=================
UI_DisplayOptionsMenu_ApplyChanges

Video Mode and Fullscreen require a vid_restart, so (unlike brightness/screen size,
which are applied live) they are committed through an Apply button.
=================
*/
static void UI_DisplayOptionsMenu_ApplyChanges( void *unused, int notification ) {
	if ( notification != QM_ACTIVATED ) {
		return;
	}

	{
		int sel = displayOptionsInfo.mode.curvalue;
		if ( sel < 0 || sel >= display_num_modes ) {
			sel = 0;
		}
		trap_Cvar_SetValue( "r_mode", display_mode_map[sel] );
	}
	trap_Cvar_SetValue( "r_fullscreen", displayOptionsInfo.fs.curvalue );
	trap_Cmd_ExecuteText( EXEC_APPEND, "vid_restart\n" );
}


/*
=================
UI_DisplayOptionsMenu_Draw

Reveal the Apply button only while the Video Mode / Fullscreen selection differs from
the values that are currently live.
=================
*/
static void UI_DisplayOptionsMenu_Draw( void ) {
	displayOptionsInfo.apply.generic.flags |= QMF_HIDDEN|QMF_INACTIVE;

	if ( displayOptionsInfo.initial_mode != displayOptionsInfo.mode.curvalue ||
		 displayOptionsInfo.initial_fs   != displayOptionsInfo.fs.curvalue ) {
		displayOptionsInfo.apply.generic.flags &= ~(QMF_HIDDEN|QMF_INACTIVE);
	}

	Menu_Draw( &displayOptionsInfo.menu );
}


/*
=================
UI_DisplayOptionsMenu_Event
=================
*/
static void UI_DisplayOptionsMenu_Event( void* ptr, int event ) {
	if( event != QM_ACTIVATED ) {
		return;
	}

	switch( ((menucommon_s*)ptr)->id ) {
	case ID_GRAPHICS:
		UI_PopMenu();
		UI_GraphicsOptionsMenu();
		break;

	case ID_DISPLAY:
		break;

	case ID_SOUND:
		UI_PopMenu();
		UI_SoundOptionsMenu();
		break;

	case ID_NETWORK:
		UI_PopMenu();
		UI_NetworkOptionsMenu();
		break;

	case ID_RAYTRACING:
		UI_PopMenu();
		UI_RaytracingOptionsMenu();
		break;

	case ID_BRIGHTNESS:
		trap_Cvar_SetValue( "r_gamma", displayOptionsInfo.brightness.curvalue / 10.0f );
		break;
	
	case ID_SCREENSIZE:
		trap_Cvar_SetValue( "cg_viewsize", displayOptionsInfo.screensize.curvalue * 10 );
		break;

	case ID_BACK:
		UI_PopMenu();
		break;
	}
}


/*
===============
UI_DisplayOptionsMenu_Init
===============
*/
static void UI_DisplayOptionsMenu_Init( void ) {
	int		y;

	memset( &displayOptionsInfo, 0, sizeof(displayOptionsInfo) );

	UI_DisplayOptionsMenu_Cache();
	displayOptionsInfo.menu.wrapAround = qtrue;
	displayOptionsInfo.menu.fullscreen = qtrue;
	displayOptionsInfo.menu.draw       = UI_DisplayOptionsMenu_Draw;

	displayOptionsInfo.banner.generic.type		= MTYPE_BTEXT;
	displayOptionsInfo.banner.generic.flags		= QMF_CENTER_JUSTIFY;
	displayOptionsInfo.banner.generic.x			= 320;
	displayOptionsInfo.banner.generic.y			= 16;
	displayOptionsInfo.banner.string			= "SYSTEM SETUP";
	displayOptionsInfo.banner.color				= color_white;
	displayOptionsInfo.banner.style				= UI_CENTER;

	displayOptionsInfo.framel.generic.type		= MTYPE_BITMAP;
	displayOptionsInfo.framel.generic.name		= ART_FRAMEL;
	displayOptionsInfo.framel.generic.flags		= QMF_INACTIVE;
	displayOptionsInfo.framel.generic.x			= 0;  
	displayOptionsInfo.framel.generic.y			= 78;
	displayOptionsInfo.framel.width				= 256;
	displayOptionsInfo.framel.height			= 329;

	displayOptionsInfo.framer.generic.type		= MTYPE_BITMAP;
	displayOptionsInfo.framer.generic.name		= ART_FRAMER;
	displayOptionsInfo.framer.generic.flags		= QMF_INACTIVE;
	displayOptionsInfo.framer.generic.x			= 376;
	displayOptionsInfo.framer.generic.y			= 76;
	displayOptionsInfo.framer.width				= 256;
	displayOptionsInfo.framer.height			= 334;

	displayOptionsInfo.graphics.generic.type		= MTYPE_PTEXT;
	displayOptionsInfo.graphics.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	displayOptionsInfo.graphics.generic.id			= ID_GRAPHICS;
	displayOptionsInfo.graphics.generic.callback	= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.graphics.generic.x			= 216;
	displayOptionsInfo.graphics.generic.y			= 240 - 2 * PROP_HEIGHT;
	displayOptionsInfo.graphics.string				= "GRAPHICS";
	displayOptionsInfo.graphics.style				= UI_RIGHT;
	displayOptionsInfo.graphics.color				= color_red;

	displayOptionsInfo.display.generic.type			= MTYPE_PTEXT;
	displayOptionsInfo.display.generic.flags		= QMF_RIGHT_JUSTIFY;
	displayOptionsInfo.display.generic.id			= ID_DISPLAY;
	displayOptionsInfo.display.generic.callback		= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.display.generic.x			= 216;
	displayOptionsInfo.display.generic.y			= 240 - PROP_HEIGHT;
	displayOptionsInfo.display.string				= "DISPLAY";
	displayOptionsInfo.display.style				= UI_RIGHT;
	displayOptionsInfo.display.color				= color_red;

	displayOptionsInfo.sound.generic.type			= MTYPE_PTEXT;
	displayOptionsInfo.sound.generic.flags			= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	displayOptionsInfo.sound.generic.id				= ID_SOUND;
	displayOptionsInfo.sound.generic.callback		= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.sound.generic.x				= 216;
	displayOptionsInfo.sound.generic.y				= 240;
	displayOptionsInfo.sound.string					= "SOUND";
	displayOptionsInfo.sound.style					= UI_RIGHT;
	displayOptionsInfo.sound.color					= color_red;

	displayOptionsInfo.network.generic.type			= MTYPE_PTEXT;
	displayOptionsInfo.network.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	displayOptionsInfo.network.generic.id			= ID_NETWORK;
	displayOptionsInfo.network.generic.callback		= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.network.generic.x			= 216;
	displayOptionsInfo.network.generic.y			= 240 + PROP_HEIGHT;
	displayOptionsInfo.network.string				= "NETWORK";
	displayOptionsInfo.network.style				= UI_RIGHT;
	displayOptionsInfo.network.color				= color_red;

	displayOptionsInfo.raytracing.generic.type		= MTYPE_PTEXT;
	displayOptionsInfo.raytracing.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	displayOptionsInfo.raytracing.generic.id		= ID_RAYTRACING;
	displayOptionsInfo.raytracing.generic.callback	= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.raytracing.generic.x			= 216;
	displayOptionsInfo.raytracing.generic.y			= 240 + 2 * PROP_HEIGHT;
	displayOptionsInfo.raytracing.string			= "RAYTRACING";
	displayOptionsInfo.raytracing.style				= UI_RIGHT;
	displayOptionsInfo.raytracing.color				= color_red;

	y = 240 - 2 * (BIGCHAR_HEIGHT+2);
	displayOptionsInfo.brightness.generic.type		= MTYPE_SLIDER;
	displayOptionsInfo.brightness.generic.name		= "Brightness:";
	displayOptionsInfo.brightness.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	displayOptionsInfo.brightness.generic.callback	= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.brightness.generic.id		= ID_BRIGHTNESS;
	displayOptionsInfo.brightness.generic.x			= 400;
	displayOptionsInfo.brightness.generic.y			= y;
	displayOptionsInfo.brightness.minvalue			= 5;
	displayOptionsInfo.brightness.maxvalue			= 20;
	if( !uis.glconfig.deviceSupportsGamma ) {
		displayOptionsInfo.brightness.generic.flags |= QMF_GRAYED;
	}

	y += BIGCHAR_HEIGHT+2;
	displayOptionsInfo.screensize.generic.type		= MTYPE_SLIDER;
	displayOptionsInfo.screensize.generic.name		= "Screen Size:";
	displayOptionsInfo.screensize.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	displayOptionsInfo.screensize.generic.callback	= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.screensize.generic.id		= ID_SCREENSIZE;
	displayOptionsInfo.screensize.generic.x			= 400;
	displayOptionsInfo.screensize.generic.y			= y;
	displayOptionsInfo.screensize.minvalue			= 3;
    displayOptionsInfo.screensize.maxvalue			= 10;

	// references/modifies "r_mode" (committed via the Apply button -> vid_restart).
	// Only desktop-supported resolutions are listed; see Screen_BuildModeList.
	Screen_BuildModeList();
	y += BIGCHAR_HEIGHT+2;
	displayOptionsInfo.mode.generic.type			= MTYPE_SPINCONTROL;
	displayOptionsInfo.mode.generic.name			= "Video Mode:";
	displayOptionsInfo.mode.generic.flags			= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	displayOptionsInfo.mode.generic.id				= ID_MODE;
	displayOptionsInfo.mode.generic.x				= 400;
	displayOptionsInfo.mode.generic.y				= y;
	displayOptionsInfo.mode.itemnames				= display_mode_names;

	// references/modifies "r_fullscreen" (committed via the Apply button -> vid_restart)
	y += BIGCHAR_HEIGHT+2;
	displayOptionsInfo.fs.generic.type				= MTYPE_SPINCONTROL;
	displayOptionsInfo.fs.generic.name				= "Fullscreen:";
	displayOptionsInfo.fs.generic.flags				= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	displayOptionsInfo.fs.generic.id				= ID_FULLSCREEN;
	displayOptionsInfo.fs.generic.x					= 400;
	displayOptionsInfo.fs.generic.y					= y;
	displayOptionsInfo.fs.itemnames					= display_enabled_names;

	displayOptionsInfo.apply.generic.type		= MTYPE_BITMAP;
	displayOptionsInfo.apply.generic.name		= ART_ACCEPT0;
	displayOptionsInfo.apply.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS|QMF_HIDDEN|QMF_INACTIVE;
	displayOptionsInfo.apply.generic.callback	= UI_DisplayOptionsMenu_ApplyChanges;
	displayOptionsInfo.apply.generic.x			= 640;
	displayOptionsInfo.apply.generic.y			= 480-64;
	displayOptionsInfo.apply.width				= 128;
	displayOptionsInfo.apply.height				= 64;
	displayOptionsInfo.apply.focuspic			= ART_ACCEPT1;

	displayOptionsInfo.back.generic.type		= MTYPE_BITMAP;
	displayOptionsInfo.back.generic.name		= ART_BACK0;
	displayOptionsInfo.back.generic.flags		= QMF_LEFT_JUSTIFY|QMF_PULSEIFFOCUS;
	displayOptionsInfo.back.generic.callback	= UI_DisplayOptionsMenu_Event;
	displayOptionsInfo.back.generic.id			= ID_BACK;
	displayOptionsInfo.back.generic.x			= 0;
	displayOptionsInfo.back.generic.y			= 480-64;
	displayOptionsInfo.back.width				= 128;
	displayOptionsInfo.back.height				= 64;
	displayOptionsInfo.back.focuspic			= ART_BACK1;

	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.banner );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.framel );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.framer );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.graphics );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.display );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.sound );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.network );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.raytracing );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.brightness );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.screensize );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.mode );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.fs );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.apply );
	Menu_AddItem( &displayOptionsInfo.menu, ( void * ) &displayOptionsInfo.back );

	displayOptionsInfo.brightness.curvalue  = trap_Cvar_VariableValue("r_gamma") * 10;
	displayOptionsInfo.screensize.curvalue  = trap_Cvar_VariableValue( "cg_viewsize")/10;

	// map the live r_mode index onto the visible spinner entry; if it is not available
	// (e.g. a custom/oversized mode), fall back to the current resolution, then to 0
	{
		int rmode = (int)trap_Cvar_VariableValue( "r_mode" );
		int i, sel = -1;

		for ( i = 0; i < display_num_modes; i++ ) {
			if ( display_mode_map[i] == rmode ) {
				sel = i;
				break;
			}
		}
		if ( sel < 0 ) {
			for ( i = 0; i < display_num_modes; i++ ) {
				const uiVidMode_t *vm = &ui_vidModes[display_mode_map[i]];
				if ( vm->width == uis.glconfig.vidWidth && vm->height == uis.glconfig.vidHeight ) {
					sel = i;
					break;
				}
			}
		}
		displayOptionsInfo.mode.curvalue = ( sel < 0 ) ? 0 : sel;
	}
	displayOptionsInfo.fs.curvalue = trap_Cvar_VariableValue( "r_fullscreen" ) != 0;

	// remember the live values so the Apply button can reveal itself on change
	displayOptionsInfo.initial_mode = displayOptionsInfo.mode.curvalue;
	displayOptionsInfo.initial_fs   = displayOptionsInfo.fs.curvalue;
}


/*
===============
UI_DisplayOptionsMenu_Cache
===============
*/
void UI_DisplayOptionsMenu_Cache( void ) {
	trap_R_RegisterShaderNoMip( ART_FRAMEL );
	trap_R_RegisterShaderNoMip( ART_FRAMER );
	trap_R_RegisterShaderNoMip( ART_BACK0 );
	trap_R_RegisterShaderNoMip( ART_BACK1 );
	trap_R_RegisterShaderNoMip( ART_ACCEPT0 );
	trap_R_RegisterShaderNoMip( ART_ACCEPT1 );
}


/*
===============
UI_DisplayOptionsMenu
===============
*/
void UI_DisplayOptionsMenu( void ) {
	UI_DisplayOptionsMenu_Init();
	UI_PushMenu( &displayOptionsInfo.menu );
	Menu_SetCursorToItem( &displayOptionsInfo.menu, &displayOptionsInfo.display );
}
