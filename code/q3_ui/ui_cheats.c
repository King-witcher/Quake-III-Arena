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

CHEATS MENU

Edits the native cheat cvars. The on/off toggles are CVAR_CHEAT, so they only
take effect with sv_cheats 1 (use devmap). The aimbot tuning sliders are plain
archived cvars and can be set anytime.

=======================================================================
*/


#include "ui_local.h"


#define ART_FRAMEL				"menu/art/frame2_l"
#define ART_FRAMER				"menu/art/frame1_r"
#define ART_BACK0				"menu/art/back_0"
#define ART_BACK1				"menu/art/back_1"

#define CHEATS_X_POS			360

#define ID_AIMBOT				140
#define ID_AIMBOTFOV			141
#define ID_AIMBOTRANGE			142
#define ID_AIMBOTSMOOTH			143
#define ID_WALLHACK				144
#define ID_BACK					145


typedef struct {
	menuframework_s		menu;

	menutext_s			banner;
	menubitmap_s		framel;
	menubitmap_s		framer;

	menuradiobutton_s	aimbot;
	menuslider_s		aimbotfov;
	menuslider_s		aimbotrange;
	menuslider_s		aimbotsmooth;
	menuradiobutton_s	wallhack;
	menutext_s			hint;
	menubitmap_s		back;
} cheats_t;

static cheats_t s_cheats;


static void Cheats_SetMenuItems( void ) {
	s_cheats.aimbot.curvalue		= trap_Cvar_VariableValue( "cg_aimbot" ) != 0;
	s_cheats.wallhack.curvalue		= trap_Cvar_VariableValue( "cg_wallhack" ) != 0;
	s_cheats.aimbotfov.curvalue		= trap_Cvar_VariableValue( "cg_aimbotFov" );
	s_cheats.aimbotrange.curvalue	= trap_Cvar_VariableValue( "cg_aimbotRange" ) / 100.0f;
	s_cheats.aimbotsmooth.curvalue	= trap_Cvar_VariableValue( "cg_aimbotSmooth" ) * 10.0f;
}


static void Cheats_Event( void *ptr, int notification ) {
	if ( notification != QM_ACTIVATED ) {
		return;
	}

	switch ( ((menucommon_s*)ptr)->id ) {
	case ID_AIMBOT:
		trap_Cvar_SetValue( "cg_aimbot", s_cheats.aimbot.curvalue );
		break;

	case ID_AIMBOTFOV:
		trap_Cvar_SetValue( "cg_aimbotFov", s_cheats.aimbotfov.curvalue );
		break;

	case ID_AIMBOTRANGE:
		// slider 0..50 -> 0..5000 game units (0 = unlimited)
		trap_Cvar_SetValue( "cg_aimbotRange", s_cheats.aimbotrange.curvalue * 100.0f );
		break;

	case ID_AIMBOTSMOOTH:
		// slider 1..10 -> 0.1..1.0 (lower = smoother slide, 1.0 = instant)
		trap_Cvar_SetValue( "cg_aimbotSmooth", s_cheats.aimbotsmooth.curvalue / 10.0f );
		break;

	case ID_WALLHACK:
		trap_Cvar_SetValue( "cg_wallhack", s_cheats.wallhack.curvalue );
		break;

	case ID_BACK:
		UI_PopMenu();
		break;
	}
}


static void Cheats_Cache( void ) {
	trap_R_RegisterShaderNoMip( ART_FRAMEL );
	trap_R_RegisterShaderNoMip( ART_FRAMER );
	trap_R_RegisterShaderNoMip( ART_BACK0 );
	trap_R_RegisterShaderNoMip( ART_BACK1 );
}


static void Cheats_MenuInit( void ) {
	int		y;

	memset( &s_cheats, 0, sizeof(cheats_t) );

	Cheats_Cache();

	s_cheats.menu.wrapAround = qtrue;
	s_cheats.menu.fullscreen = qtrue;

	s_cheats.banner.generic.type	= MTYPE_BTEXT;
	s_cheats.banner.generic.x		= 320;
	s_cheats.banner.generic.y		= 16;
	s_cheats.banner.string			= "TRAPACAS";
	s_cheats.banner.color			= color_white;
	s_cheats.banner.style			= UI_CENTER;

	s_cheats.framel.generic.type	= MTYPE_BITMAP;
	s_cheats.framel.generic.name	= ART_FRAMEL;
	s_cheats.framel.generic.flags	= QMF_INACTIVE;
	s_cheats.framel.generic.x		= 0;
	s_cheats.framel.generic.y		= 78;
	s_cheats.framel.width			= 256;
	s_cheats.framel.height			= 329;

	s_cheats.framer.generic.type	= MTYPE_BITMAP;
	s_cheats.framer.generic.name	= ART_FRAMER;
	s_cheats.framer.generic.flags	= QMF_INACTIVE;
	s_cheats.framer.generic.x		= 376;
	s_cheats.framer.generic.y		= 76;
	s_cheats.framer.width			= 256;
	s_cheats.framer.height			= 334;

	y = 168;
	s_cheats.aimbot.generic.type		= MTYPE_RADIOBUTTON;
	s_cheats.aimbot.generic.name		= "Aimbot (segure ALT):";
	s_cheats.aimbot.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	s_cheats.aimbot.generic.callback	= Cheats_Event;
	s_cheats.aimbot.generic.id			= ID_AIMBOT;
	s_cheats.aimbot.generic.x			= CHEATS_X_POS;
	s_cheats.aimbot.generic.y			= y;

	y += BIGCHAR_HEIGHT+2;
	s_cheats.aimbotfov.generic.type		= MTYPE_SLIDER;
	s_cheats.aimbotfov.generic.name		= "Aimbot FOV:";
	s_cheats.aimbotfov.generic.flags	= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	s_cheats.aimbotfov.generic.callback	= Cheats_Event;
	s_cheats.aimbotfov.generic.id		= ID_AIMBOTFOV;
	s_cheats.aimbotfov.generic.x		= CHEATS_X_POS;
	s_cheats.aimbotfov.generic.y		= y;
	s_cheats.aimbotfov.minvalue			= 0;
	s_cheats.aimbotfov.maxvalue			= 180;

	y += BIGCHAR_HEIGHT+2;
	s_cheats.aimbotrange.generic.type		= MTYPE_SLIDER;
	s_cheats.aimbotrange.generic.name		= "Alcance do Aimbot:";
	s_cheats.aimbotrange.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	s_cheats.aimbotrange.generic.callback	= Cheats_Event;
	s_cheats.aimbotrange.generic.id			= ID_AIMBOTRANGE;
	s_cheats.aimbotrange.generic.x			= CHEATS_X_POS;
	s_cheats.aimbotrange.generic.y			= y;
	s_cheats.aimbotrange.minvalue			= 0;
	s_cheats.aimbotrange.maxvalue			= 50;

	y += BIGCHAR_HEIGHT+2;
	s_cheats.aimbotsmooth.generic.type		= MTYPE_SLIDER;
	s_cheats.aimbotsmooth.generic.name		= "Suavizacao do Aimbot:";
	s_cheats.aimbotsmooth.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	s_cheats.aimbotsmooth.generic.callback	= Cheats_Event;
	s_cheats.aimbotsmooth.generic.id		= ID_AIMBOTSMOOTH;
	s_cheats.aimbotsmooth.generic.x			= CHEATS_X_POS;
	s_cheats.aimbotsmooth.generic.y			= y;
	s_cheats.aimbotsmooth.minvalue			= 1;
	s_cheats.aimbotsmooth.maxvalue			= 10;

	y += BIGCHAR_HEIGHT+2;
	s_cheats.wallhack.generic.type		= MTYPE_RADIOBUTTON;
	s_cheats.wallhack.generic.name		= "Wallhack (ESP):";
	s_cheats.wallhack.generic.flags		= QMF_PULSEIFFOCUS|QMF_SMALLFONT;
	s_cheats.wallhack.generic.callback	= Cheats_Event;
	s_cheats.wallhack.generic.id		= ID_WALLHACK;
	s_cheats.wallhack.generic.x			= CHEATS_X_POS;
	s_cheats.wallhack.generic.y			= y;

	y += 2*BIGCHAR_HEIGHT;
	s_cheats.hint.generic.type	= MTYPE_PTEXT;
	s_cheats.hint.generic.flags	= QMF_INACTIVE;
	s_cheats.hint.generic.x		= 320;
	s_cheats.hint.generic.y		= y;
	s_cheats.hint.string		= "as opcoes requerem sv_cheats 1 (use devmap)";
	s_cheats.hint.color			= color_red;
	s_cheats.hint.style			= UI_CENTER|UI_SMALLFONT;

	s_cheats.back.generic.type		= MTYPE_BITMAP;
	s_cheats.back.generic.name		= ART_BACK0;
	s_cheats.back.generic.flags		= QMF_LEFT_JUSTIFY|QMF_PULSEIFFOCUS;
	s_cheats.back.generic.callback	= Cheats_Event;
	s_cheats.back.generic.id		= ID_BACK;
	s_cheats.back.generic.x			= 0;
	s_cheats.back.generic.y			= 480-64;
	s_cheats.back.width				= 128;
	s_cheats.back.height			= 64;
	s_cheats.back.focuspic			= ART_BACK1;

	Menu_AddItem( &s_cheats.menu, &s_cheats.banner );
	Menu_AddItem( &s_cheats.menu, &s_cheats.framel );
	Menu_AddItem( &s_cheats.menu, &s_cheats.framer );
	Menu_AddItem( &s_cheats.menu, &s_cheats.aimbot );
	Menu_AddItem( &s_cheats.menu, &s_cheats.aimbotfov );
	Menu_AddItem( &s_cheats.menu, &s_cheats.aimbotrange );
	Menu_AddItem( &s_cheats.menu, &s_cheats.aimbotsmooth );
	Menu_AddItem( &s_cheats.menu, &s_cheats.wallhack );
	Menu_AddItem( &s_cheats.menu, &s_cheats.hint );
	Menu_AddItem( &s_cheats.menu, &s_cheats.back );

	Cheats_SetMenuItems();
}


/*
===============
UI_CheatsMenu
===============
*/
void UI_CheatsMenu( void ) {
	Cheats_MenuInit();
	UI_PushMenu( &s_cheats.menu );
}
