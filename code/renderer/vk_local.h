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
// vk_local.h -- internal types and declarations shared by the Vulkan backend
// files (vk_*.c) and the Win32 surface layer (win_vk.c).  This header pulls in
// the Vulkan headers, so it is deliberately *not* included by tr_local.h or any
// API-agnostic renderer file.
//
#ifndef __VK_LOCAL_H__
#define __VK_LOCAL_H__

#include "tr_local.h"
#include "qvk.h"

// Number of frames the CPU may have in flight before waiting on the GPU.
#define VK_NUM_FRAMES		2

// A swapchain rarely exceeds 3-4 images; cap generously.
#define MAX_SWAPCHAIN_IMAGES	8

// We target Vulkan 1.3 core (dynamic rendering, no legacy render-pass objects).
#define VK_TARGET_API_VERSION	VK_API_VERSION_1_3

//
// vk -- the single global holding all Vulkan device-level state.  Cleared to
// zero on shutdown so a stale handle is never reused across a vid_restart.
//
typedef struct {
	qboolean			initialized;

	// instance / debug
	uint32_t			instanceApiVersion;
	VkInstance			instance;
	qboolean			validation;
	VkDebugUtilsMessengerEXT debugMessenger;

	// physical + logical device
	VkPhysicalDevice	physicalDevice;
	VkPhysicalDeviceProperties			devProps;
	VkPhysicalDeviceMemoryProperties	memProps;
	VkPhysicalDeviceFeatures			devFeatures;
	VkDevice			device;
	uint32_t			graphicsFamily;
	uint32_t			presentFamily;
	VkQueue				graphicsQueue;
	VkQueue				presentQueue;

	// presentation surface (created by the platform layer)
	VkSurfaceKHR		surface;
	VkSurfaceFormatKHR	surfaceFormat;
	VkPresentModeKHR	presentMode;

	// swapchain + its color images
	VkSwapchainKHR		swapchain;
	VkExtent2D			extent;
	uint32_t			imageCount;
	VkImage				swapchainImages[MAX_SWAPCHAIN_IMAGES];
	VkImageView			swapchainViews[MAX_SWAPCHAIN_IMAGES];

	// shared depth/stencil attachment
	VkFormat			depthFormat;
	VkImage				depthImage;
	VkDeviceMemory		depthMemory;
	VkImageView			depthView;

	// per-frame-in-flight objects
	VkCommandPool		commandPool;
	VkCommandBuffer		commandBuffers[VK_NUM_FRAMES];
	VkSemaphore			imageAcquired[VK_NUM_FRAMES];
	VkSemaphore			renderComplete[VK_NUM_FRAMES];
	VkFence				frameFence[VK_NUM_FRAMES];

	// live frame state
	int					frameIndex;			// 0..VK_NUM_FRAMES-1
	uint32_t			swapchainIndex;		// acquired image for this frame
	VkCommandBuffer		cmd;				// active primary command buffer
	qboolean			frameStarted;		// between begin and present
	qboolean			swapchainValid;		// false => needs (re)creation
} vk_t;

extern vk_t	vk;

//
// result checking
//
const char *VK_ResultString( VkResult result );

#define VK_CHECK( call )                                                        \
	do {                                                                        \
		VkResult _vkr = (call);                                                 \
		if ( _vkr != VK_SUCCESS ) {                                             \
			ri.Error( ERR_FATAL, "Vulkan error %s returned by %s (%s:%d)",      \
				VK_ResultString( _vkr ), #call, __FILE__, __LINE__ );           \
		}                                                                       \
	} while ( 0 )

//
// vk_instance.c
//
qboolean	VK_Init( void );					// returns qfalse => caller falls back to GL
void		VK_Shutdown( qboolean destroyWindow );
void		VK_GfxInfo( void );
uint32_t	VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags properties );

//
// vk_swapchain.c
//
qboolean	VK_CreateSwapchain( void );
void		VK_DestroySwapchain( void );
qboolean	VK_RecreateSwapchain( void );
qboolean	VK_CreateFrameResources( void );
void		VK_DestroyFrameResources( void );

//
// vk_backend.c
//
void		VKBE_Install( backend_t *b );		// fill the dispatch table with VK_* leaves

//
// win_vk.c -- Win32 platform / surface layer (parallel to win_glimp.c)
//
qboolean	VKimp_LoadLibrary( void );			// LoadLibrary("vulkan-1.dll"), bind vkGetInstanceProcAddr
void		VKimp_FreeLibrary( void );
qboolean	VKimp_CreateWindow( void );			// create the game window, fill glConfig dims
void		VKimp_DestroyWindow( void );
qboolean	VKimp_CreateSurface( VkInstance instance, VkSurfaceKHR *surface );
// Append the platform surface extension(s) to a caller-owned name list.
void		VKimp_GetRequiredInstanceExtensions( const char **names, int *count, int maxCount );

#endif // __VK_LOCAL_H__
