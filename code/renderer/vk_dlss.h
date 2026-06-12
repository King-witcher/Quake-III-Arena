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
// vk_dlss.h -- NVIDIA DLSS (Deep Learning Super Sampling) integration for the
// Vulkan backend.
//
// DLSS renders the 3D scene into a LOWER-resolution offscreen colour target
// (plus a matching depth buffer and a screen-space motion-vector buffer), then
// the NGX runtime upscales that to the display resolution with a temporal
// neural network.  This file is the thin wrapper the rest of the Vulkan backend
// talks to; all NVIDIA NGX SDK calls are confined to vk_dlss.c.
//
// The real DLSS path is compiled only when USE_DLSS is defined AND the NGX SDK
// headers are vendored under renderer/nvsdk_ngx/include (see
// DLSS_VULKAN_REFERENCE.md).  Without the SDK the module still builds: it
// reports "unavailable", but VK_DLSS_RenderResolution() still returns the
// per-mode lower render resolution, so r_dlss transparently degrades to
// "render low, linear-upscale to display" -- a real, visible performance win
// even on non-RTX hardware.
//
#ifndef __VK_DLSS_H__
#define __VK_DLSS_H__

#include "vk_local.h"

const char	*VK_DLSS_ModeName( int mode );	// "Off"/"Quality"/.../"Ultra Performance"

// Mirrors the r_dlss cvar.  The numeric values are part of the cvar/menu ABI.
typedef enum {
	VK_DLSS_OFF			= 0,
	VK_DLSS_QUALITY		= 1,	// render = display / 1.5
	VK_DLSS_BALANCED	= 2,	// render = display / 1.724
	VK_DLSS_PERFORMANCE	= 3,	// render = display / 2.0
	VK_DLSS_ULTRA_PERF	= 4		// render = display / 3.0
} vkDlssMode_t;

// Extension discovery -- must run BEFORE the VkInstance / VkDevice is created.
// Each appends the NGX-required extension names to a caller-owned list (never
// exceeding maxCount).  No-ops when DLSS is not compiled in.
void		VK_DLSS_GetRequiredInstanceExtensions( const char **names, int *count, int maxCount );
void		VK_DLSS_GetRequiredDeviceExtensions( VkPhysicalDevice phys, const char **names, int *count, int maxCount );

// Bring NGX up / down on the already-created instance + device.  Safe to call
// unconditionally; VK_DLSS_Available() reflects the result.
void		VK_DLSS_Init( void );
void		VK_DLSS_Shutdown( void );

// True only when the real NGX super-resolution path is live (compiled in, NGX
// initialised, and the GPU/driver report DLSS support).
qboolean	VK_DLSS_Available( void );

// Map r_dlss to a sub-display render resolution.  Always succeeds for a non-Off
// mode (uses the published per-mode ratio, or the NGX optimal query when the SDK
// is present); returns qfalse for VK_DLSS_OFF.
qboolean	VK_DLSS_RenderResolution( vkDlssMode_t mode, uint32_t outW, uint32_t outH,
								   uint32_t *renderW, uint32_t *renderH );

// (Re)create the DLSS feature for the given sizes.  Returns qfalse when the real
// NGX path is unavailable (caller then upscales with a plain linear blit).
qboolean	VK_DLSS_CreateFeature( VkCommandBuffer cmd, vkDlssMode_t mode,
								uint32_t renderW, uint32_t renderH,
								uint32_t displayW, uint32_t displayH );
void		VK_DLSS_ReleaseFeature( void );

// Per-frame evaluate: colour(render-res) + depth + motion vectors -> output
// (display-res).  jitterX/Y are in render-res pixels; reset != 0 throws away the
// temporal history (camera cut / teleport / first frame).  Returns qfalse when
// the real path is unavailable.
qboolean	VK_DLSS_Evaluate( VkCommandBuffer cmd,
							VkImage colorImg, VkImageView colorView,
							VkImage depthImg, VkImageView depthView,
							VkImage mvImg,    VkImageView mvView,
							VkImage outImg,   VkImageView outView,
							uint32_t renderW, uint32_t renderH,
							uint32_t displayW, uint32_t displayH,
							float jitterX, float jitterY, qboolean reset );

// Halton(2,3) sub-pixel jitter for frame index `frame`, in render-res pixel
// space ([-0.5,+0.5]).  The phase count scales with the upscale ratio, exactly
// as the DLSS programming guide recommends.
void		VK_DLSS_Jitter( int frame, uint32_t renderW, uint32_t displayW, float *jx, float *jy );

#endif // __VK_DLSS_H__
