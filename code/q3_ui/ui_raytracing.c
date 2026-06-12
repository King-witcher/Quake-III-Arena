/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.
===========================================================================
*/
//
/*
=======================================================================

RAY TRACING OPTIONS MENU

Dedicated System-setup tab for the hardware ray-traced lighting (Vulkan + RTX).
"Ray Tracing" itself is a latched cvar (r_raytracing) so it is committed through
the Apply button -> vid_restart; the tuning knobs (GI, intensity, ambient, rays)
are live cvars applied immediately as they change.

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
#define ID_RAYTRACING		14
#define ID_RT_ENABLE		15
#define ID_RT_GI			16
#define ID_RT_GIINTENSITY	17
#define ID_RT_AMBIENT		18
#define ID_RT_RAYS			19
#define ID_BACK				20


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

	menulist_s		enable;			// r_raytracing (latched -> Apply)
	menulist_s		gi;				// r_rtGI
	menuslider_s	giIntensity;	// r_rtGIIntensity * 10
	menuslider_s	ambient;		// r_rtAmbientScale * 10
	menuslider_s	rays;			// r_rtRays

	menubitmap_s	apply;
	menubitmap_s	back;

	int				initial_enable;
} raytracingOptionsInfo_t;

static raytracingOptionsInfo_t	rtOptionsInfo;

static const char *rt_onoff_names[] = { "Off", "On", 0 };


/*
=================
UI_RaytracingOptionsMenu_ApplyChanges

r_raytracing is latched, so toggling it needs a vid_restart.
=================
*/
static void UI_RaytracingOptionsMenu_ApplyChanges( void *unused, int notification ) {
	if ( notification != QM_ACTIVATED ) {
		return;
	}
	trap_Cvar_SetValue( "r_raytracing", rtOptionsInfo.enable.curvalue );
	trap_Cmd_ExecuteText( EXEC_APPEND, "vid_restart\n" );
}


/*
=================
UI_RaytracingOptionsMenu_Draw

Reveal the Apply button only while the (latched) On/Off selection differs from
what is currently live.
=================
*/
static void UI_RaytracingOptionsMenu_Draw( void ) {
	rtOptionsInfo.apply.generic.flags |= QMF_HIDDEN|QMF_INACTIVE;
	if ( rtOptionsInfo.initial_enable != rtOptionsInfo.enable.curvalue ) {
		rtOptionsInfo.apply.generic.flags &= ~(QMF_HIDDEN|QMF_INACTIVE);
	}
	Menu_Draw( &rtOptionsInfo.menu );
}


/*
=================
UI_RaytracingOptionsMenu_Event
=================
*/
static void UI_RaytracingOptionsMenu_Event( void* ptr, int event ) {
	if ( event != QM_ACTIVATED ) {
		return;
	}

	switch ( ((menucommon_s*)ptr)->id ) {
	case ID_GRAPHICS:
		UI_PopMenu();
		UI_GraphicsOptionsMenu();
		break;

	case ID_DISPLAY:
		UI_PopMenu();
		UI_DisplayOptionsMenu();
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
		break;

	// live tuning cvars (take effect next frame, no restart)
	case ID_RT_GI:
		trap_Cvar_SetValue( "r_rtGI", rtOptionsInfo.gi.curvalue );
		break;
	case ID_RT_GIINTENSITY:
		trap_Cvar_SetValue( "r_rtGIIntensity", rtOptionsInfo.giIntensity.curvalue / 10.0f );
		break;
	case ID_RT_AMBIENT:
		trap_Cvar_SetValue( "r_rtAmbientScale", rtOptionsInfo.ambient.curvalue / 10.0f );
		break;
	case ID_RT_RAYS:
		trap_Cvar_SetValue( "r_rtRays", rtOptionsInfo.rays.curvalue );
		break;

	case ID_BACK:
		UI_PopMenu();
		break;
	}
}


/*
===============
UI_RaytracingOptionsMenu_Init
===============
*/
static void UI_RaytracingOptionsMenu_Init( void ) {
	int		y;
	qboolean vulkan;

	memset( &rtOptionsInfo, 0, sizeof( rtOptionsInfo ) );

	UI_RaytracingOptionsMenu_Cache();
	rtOptionsInfo.menu.wrapAround = qtrue;
	rtOptionsInfo.menu.fullscreen = qtrue;
	rtOptionsInfo.menu.draw       = UI_RaytracingOptionsMenu_Draw;

	rtOptionsInfo.banner.generic.type	= MTYPE_BTEXT;
	rtOptionsInfo.banner.generic.flags	= QMF_CENTER_JUSTIFY;
	rtOptionsInfo.banner.generic.x		= 320;
	rtOptionsInfo.banner.generic.y		= 16;
	rtOptionsInfo.banner.string			= "SYSTEM SETUP";
	rtOptionsInfo.banner.color			= color_white;
	rtOptionsInfo.banner.style			= UI_CENTER;

	rtOptionsInfo.framel.generic.type	= MTYPE_BITMAP;
	rtOptionsInfo.framel.generic.name	= ART_FRAMEL;
	rtOptionsInfo.framel.generic.flags	= QMF_INACTIVE;
	rtOptionsInfo.framel.generic.x		= 0;
	rtOptionsInfo.framel.generic.y		= 78;
	rtOptionsInfo.framel.width			= 256;
	rtOptionsInfo.framel.height			= 329;

	rtOptionsInfo.framer.generic.type	= MTYPE_BITMAP;
	rtOptionsInfo.framer.generic.name	= ART_FRAMER;
	rtOptionsInfo.framer.generic.flags	= QMF_INACTIVE;
	rtOptionsInfo.framer.generic.x		= 376;
	rtOptionsInfo.framer.generic.y		= 76;
	rtOptionsInfo.framer.width			= 256;
	rtOptionsInfo.framer.height			= 334;

	// tab strip: GRAPHICS / DISPLAY / SOUND / NETWORK / RAYTRACING (this page is red+static)
	rtOptionsInfo.graphics.generic.type		= MTYPE_PTEXT;
	rtOptionsInfo.graphics.generic.flags	= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	rtOptionsInfo.graphics.generic.id		= ID_GRAPHICS;
	rtOptionsInfo.graphics.generic.callback	= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.graphics.generic.x		= 216;
	rtOptionsInfo.graphics.generic.y		= 240 - 2 * PROP_HEIGHT;
	rtOptionsInfo.graphics.string			= "GRAPHICS";
	rtOptionsInfo.graphics.style			= UI_RIGHT;
	rtOptionsInfo.graphics.color			= color_red;

	rtOptionsInfo.display.generic.type		= MTYPE_PTEXT;
	rtOptionsInfo.display.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	rtOptionsInfo.display.generic.id		= ID_DISPLAY;
	rtOptionsInfo.display.generic.callback	= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.display.generic.x			= 216;
	rtOptionsInfo.display.generic.y			= 240 - PROP_HEIGHT;
	rtOptionsInfo.display.string			= "DISPLAY";
	rtOptionsInfo.display.style				= UI_RIGHT;
	rtOptionsInfo.display.color				= color_red;

	rtOptionsInfo.sound.generic.type		= MTYPE_PTEXT;
	rtOptionsInfo.sound.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	rtOptionsInfo.sound.generic.id			= ID_SOUND;
	rtOptionsInfo.sound.generic.callback	= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.sound.generic.x			= 216;
	rtOptionsInfo.sound.generic.y			= 240;
	rtOptionsInfo.sound.string				= "SOUND";
	rtOptionsInfo.sound.style				= UI_RIGHT;
	rtOptionsInfo.sound.color				= color_red;

	rtOptionsInfo.network.generic.type		= MTYPE_PTEXT;
	rtOptionsInfo.network.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS;
	rtOptionsInfo.network.generic.id		= ID_NETWORK;
	rtOptionsInfo.network.generic.callback	= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.network.generic.x			= 216;
	rtOptionsInfo.network.generic.y			= 240 + PROP_HEIGHT;
	rtOptionsInfo.network.string			= "NETWORK";
	rtOptionsInfo.network.style				= UI_RIGHT;
	rtOptionsInfo.network.color				= color_red;

	rtOptionsInfo.raytracing.generic.type	= MTYPE_PTEXT;
	rtOptionsInfo.raytracing.generic.flags	= QMF_RIGHT_JUSTIFY;
	rtOptionsInfo.raytracing.generic.id		= ID_RAYTRACING;
	rtOptionsInfo.raytracing.generic.callback = UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.raytracing.generic.x		= 216;
	rtOptionsInfo.raytracing.generic.y		= 240 + 2 * PROP_HEIGHT;
	rtOptionsInfo.raytracing.string			= "RAYTRACING";
	rtOptionsInfo.raytracing.style			= UI_RIGHT;
	rtOptionsInfo.raytracing.color			= color_red;

	// controls
	y = 240 - 2 * ( BIGCHAR_HEIGHT + 2 );
	rtOptionsInfo.enable.generic.type		= MTYPE_SPINCONTROL;
	rtOptionsInfo.enable.generic.name		= "Ray Tracing:";
	rtOptionsInfo.enable.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	rtOptionsInfo.enable.generic.callback	= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.enable.generic.id			= ID_RT_ENABLE;
	rtOptionsInfo.enable.generic.x			= 400;
	rtOptionsInfo.enable.generic.y			= y;
	rtOptionsInfo.enable.itemnames			= rt_onoff_names;

	y += 2 * ( BIGCHAR_HEIGHT + 2 );
	rtOptionsInfo.gi.generic.type			= MTYPE_SPINCONTROL;
	rtOptionsInfo.gi.generic.name			= "Color Bleed (GI):";
	rtOptionsInfo.gi.generic.flags			= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	rtOptionsInfo.gi.generic.callback		= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.gi.generic.id				= ID_RT_GI;
	rtOptionsInfo.gi.generic.x				= 400;
	rtOptionsInfo.gi.generic.y				= y;
	rtOptionsInfo.gi.itemnames				= rt_onoff_names;

	y += BIGCHAR_HEIGHT + 2;
	rtOptionsInfo.giIntensity.generic.type	= MTYPE_SLIDER;
	rtOptionsInfo.giIntensity.generic.name	= "GI Intensity:";
	rtOptionsInfo.giIntensity.generic.flags	= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	rtOptionsInfo.giIntensity.generic.callback = UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.giIntensity.generic.id	= ID_RT_GIINTENSITY;
	rtOptionsInfo.giIntensity.generic.x		= 400;
	rtOptionsInfo.giIntensity.generic.y		= y;
	rtOptionsInfo.giIntensity.minvalue		= 0;
	rtOptionsInfo.giIntensity.maxvalue		= 40;	// 0.0 .. 4.0

	y += BIGCHAR_HEIGHT + 2;
	rtOptionsInfo.ambient.generic.type		= MTYPE_SLIDER;
	rtOptionsInfo.ambient.generic.name		= "Ambient Base:";
	rtOptionsInfo.ambient.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	rtOptionsInfo.ambient.generic.callback	= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.ambient.generic.id		= ID_RT_AMBIENT;
	rtOptionsInfo.ambient.generic.x			= 400;
	rtOptionsInfo.ambient.generic.y			= y;
	rtOptionsInfo.ambient.minvalue			= 0;
	rtOptionsInfo.ambient.maxvalue			= 15;	// 0.0 .. 1.5

	y += BIGCHAR_HEIGHT + 2;
	rtOptionsInfo.rays.generic.type			= MTYPE_SLIDER;
	rtOptionsInfo.rays.generic.name			= "Quality (Rays):";
	rtOptionsInfo.rays.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	rtOptionsInfo.rays.generic.callback		= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.rays.generic.id			= ID_RT_RAYS;
	rtOptionsInfo.rays.generic.x			= 400;
	rtOptionsInfo.rays.generic.y			= y;
	rtOptionsInfo.rays.minvalue				= 1;
	rtOptionsInfo.rays.maxvalue				= 16;

	rtOptionsInfo.apply.generic.type		= MTYPE_BITMAP;
	rtOptionsInfo.apply.generic.name		= ART_ACCEPT0;
	rtOptionsInfo.apply.generic.flags		= QMF_RIGHT_JUSTIFY|QMF_PULSEIFFOCUS|QMF_HIDDEN|QMF_INACTIVE;
	rtOptionsInfo.apply.generic.callback	= UI_RaytracingOptionsMenu_ApplyChanges;
	rtOptionsInfo.apply.generic.x			= 640;
	rtOptionsInfo.apply.generic.y			= 480-64;
	rtOptionsInfo.apply.width				= 128;
	rtOptionsInfo.apply.height				= 64;
	rtOptionsInfo.apply.focuspic			= ART_ACCEPT1;

	rtOptionsInfo.back.generic.type			= MTYPE_BITMAP;
	rtOptionsInfo.back.generic.name			= ART_BACK0;
	rtOptionsInfo.back.generic.flags		= QMF_LEFT_JUSTIFY|QMF_PULSEIFFOCUS;
	rtOptionsInfo.back.generic.callback		= UI_RaytracingOptionsMenu_Event;
	rtOptionsInfo.back.generic.id			= ID_BACK;
	rtOptionsInfo.back.generic.x			= 0;
	rtOptionsInfo.back.generic.y			= 480-64;
	rtOptionsInfo.back.width				= 128;
	rtOptionsInfo.back.height				= 64;
	rtOptionsInfo.back.focuspic				= ART_BACK1;

	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.banner );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.framel );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.framer );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.graphics );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.display );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.sound );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.network );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.raytracing );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.enable );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.gi );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.giIntensity );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.ambient );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.rays );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.apply );
	Menu_AddItem( &rtOptionsInfo.menu, &rtOptionsInfo.back );

	// load live cvar values
	rtOptionsInfo.enable.curvalue      = trap_Cvar_VariableValue( "r_raytracing" ) != 0;
	rtOptionsInfo.gi.curvalue          = trap_Cvar_VariableValue( "r_rtGI" ) != 0;
	rtOptionsInfo.giIntensity.curvalue = trap_Cvar_VariableValue( "r_rtGIIntensity" ) * 10.0f;
	rtOptionsInfo.ambient.curvalue     = trap_Cvar_VariableValue( "r_rtAmbientScale" ) * 10.0f;
	rtOptionsInfo.rays.curvalue        = trap_Cvar_VariableValue( "r_rtRays" );
	rtOptionsInfo.initial_enable       = rtOptionsInfo.enable.curvalue;

	// ray tracing is Vulkan + RTX only; gray the enable toggle under OpenGL
	vulkan = ( trap_Cvar_VariableValue( "r_renderapi" ) != 0 );
	if ( !vulkan ) {
		rtOptionsInfo.enable.generic.flags |= QMF_GRAYED;
	}
}


/*
===============
UI_RaytracingOptionsMenu_Cache
===============
*/
void UI_RaytracingOptionsMenu_Cache( void ) {
	trap_R_RegisterShaderNoMip( ART_FRAMEL );
	trap_R_RegisterShaderNoMip( ART_FRAMER );
	trap_R_RegisterShaderNoMip( ART_BACK0 );
	trap_R_RegisterShaderNoMip( ART_BACK1 );
	trap_R_RegisterShaderNoMip( ART_ACCEPT0 );
	trap_R_RegisterShaderNoMip( ART_ACCEPT1 );
}


/*
===============
UI_RaytracingOptionsMenu
===============
*/
void UI_RaytracingOptionsMenu( void ) {
	UI_RaytracingOptionsMenu_Init();
	UI_PushMenu( &rtOptionsInfo.menu );
	Menu_SetCursorToItem( &rtOptionsInfo.menu, &rtOptionsInfo.raytracing );
}
