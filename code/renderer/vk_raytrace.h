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
// vk_raytrace.h -- hardware ray-traced lighting for the Vulkan backend.
//
// Quake III is a multi-pass forward renderer, so we ray-trace DEFERRED: the
// existing scene pass writes a small G-buffer (world normal + albedo, alongside
// depth and the normal lit colour) and a single compute pass then traces rays
// once per screen pixel against an acceleration structure built from the world
// geometry.  This uses VK_KHR_ray_query (rayQueryEXT inside a compute shader),
// which needs no ray-tracing pipeline / shader binding table and so drops into
// the existing dispatch model cleanly.
//
// The lighting model keeps Q3's baked lightmaps as a dimmed GLOBAL AMBIENT BASE
// (so nothing is ever fully black) and adds, on top:
//   * ray-traced shadows for the dynamic lights, and
//   * a 1-bounce diffuse indirect term (colour bleeding -- a red surface tints
//     the floor around it red).
//
// Everything here is gated on the r_raytracing cvar AND on the device actually
// supporting ray query + acceleration structures + buffer device address.  When
// unsupported the module reports inactive and the backend renders exactly as it
// did before (same graceful-fallback contract as vk_dlss.c).
//
#ifndef __VK_RAYTRACE_H__
#define __VK_RAYTRACE_H__

#include "vk_local.h"

// Pure device-capability query (ray query + acceleration structure + buffer
// device address feature + the required extensions).  Independent of the cvar;
// used to set vk.rtxSupported during physical-device selection and to gate the
// UI option.
qboolean	VK_RT_DeviceSupported( VkPhysicalDevice phys );

// Append the RT device extensions this device actually exposes to a caller-owned
// list (never exceeding maxCount).  Must run before vkCreateDevice.
void		VK_RT_GetRequiredDeviceExtensions( VkPhysicalDevice phys, const char **names, int *count, int maxCount );

// Fill a pNext feature chain enabling ray query + acceleration structure +
// buffer device address.  Returns the head to splice into VkDeviceCreateInfo (or
// the passed-in tail when RT is not being enabled).  The three feature structs
// are caller-owned storage (kept alive until vkCreateDevice returns).
void		*VK_RT_BuildDeviceFeatureChain(
				void *tail,
				VkPhysicalDeviceAccelerationStructureFeaturesKHR *asf,
				VkPhysicalDeviceRayQueryFeaturesKHR *rqf,
				VkPhysicalDeviceBufferDeviceAddressFeatures *bdaf );

// Bring the RT subsystem up / down on the already-created device.  Init resolves
// the KHR entry points (and, from Phase 1 on, builds the per-frame compute
// resources); on any failure it disables RT and leaves the raster path intact.
void		VK_RT_Init( void );
void		VK_RT_Shutdown( void );

// True only when RT is enabled, supported and fully initialised -- i.e. the
// backend should run the ray-traced lighting path this frame.
qboolean	VK_RT_Active( void );

// Per-swapchain RT render targets + descriptors (rtColor storage image, depth
// sample views, samplers, compute pipeline/sets).  Created/destroyed alongside
// the swapchain in vk_swapchain.c.  No-ops unless vk.rtxEnabled.
qboolean	VK_RT_CreateTargets( void );
void		VK_RT_DestroyTargets( void );

// Deferred lighting resolve: end-of-3D-scene hook.  Runs the ray-query compute
// pass over the offscreen scene colour + depth and blits the result into the
// swapchain, leaving it in COLOR_ATTACHMENT_OPTIMAL (ready for the 2D overlay
// pass / present).  Records into vk.cmd.  Called only when vk.rtxEnabled.
void		VK_RT_Resolve( void );

// Capture the current 3D view's projection + view matrices and eye position so the
// deferred pass can reconstruct world position from depth.  Called from
// VK_SetViewport each 3D view; the last capture before the 2D overlay is the main
// scene view.  Matrices are column-major (OpenGL/Q3 convention).
void		VK_RT_SetCamera( const float *projMatrix, const float *viewMatrix, const float *eye );

#endif // __VK_RAYTRACE_H__
