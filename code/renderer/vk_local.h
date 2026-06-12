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

// Per-frame host-visible streaming buffers for the dynamic tess geometry.
#define VK_VERTEX_BUFFER_SIZE	( 16 * 1024 * 1024 )
#define VK_INDEX_BUFFER_SIZE	(  4 * 1024 * 1024 )

// Interleaved vertex written from the tess arrays; matches the GL client arrays
// (position, per-vertex RGBA, two texcoord sets).  32 bytes, 4-byte aligned.
typedef struct {
	float		xyz[3];
	byte		color[4];
	float		tc0[2];
	float		tc1[2];
} vkVertex_t;

// Which SPIR-V pair a pipeline uses.
typedef enum {
	VK_SHADER_SINGLE,			// vertexColor * tex0  (+ optional alpha test)
	VK_SHADER_MULTI,			// two-texture combine (Phase 5)
	VK_SHADER_COUNT
} vkShaderType_t;

// Per-image GPU resources; image_t.vkData points at one of these.
typedef struct {
	VkImage			image;
	VkDeviceMemory	memory;
	VkImageView		view;
	VkSampler		sampler;		// from the sampler cache (not owned)
	VkDescriptorSet	descriptor;		// set 0: combined image sampler
} vkimage_t;

// Key that uniquely identifies a graphics pipeline (hashed + cached).
typedef struct {
	unsigned		stateBits;		// GLS_* (blend/depth/polymode/atest)
	byte			cullType;		// cullType_t
	byte			mirror;			// backEnd.viewParms.isMirror (flips frontFace)
	byte			shaderType;		// vkShaderType_t
	byte			multitexEnv;	// GL_MODULATE/GL_ADD/GL_REPLACE for unit 1
	byte			polygonOffset;	// shader_t.polygonOffset -> depthBias
	byte			pad[3];
} vkPipelineKey_t;

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

	// per-frame host-visible vertex/index streaming rings (vk_memory.c)
	VkBuffer			vertexBuffer[VK_NUM_FRAMES];
	VkDeviceMemory		vertexMemory[VK_NUM_FRAMES];
	byte				*vertexMapped[VK_NUM_FRAMES];
	VkBuffer			indexBuffer[VK_NUM_FRAMES];
	VkDeviceMemory		indexMemory[VK_NUM_FRAMES];
	byte				*indexMapped[VK_NUM_FRAMES];
	uint32_t			vertexOffset;		// bytes used this frame
	uint32_t			indexOffset;

	// pipelines / descriptors / shaders (vk_pipeline.c)
	VkDescriptorSetLayout	descriptorSetLayout;	// set N: one combined image sampler
	VkPipelineLayout		pipelineLayout[3];		// index = number of descriptor sets (1 or 2)
	VkPipelineCache			pipelineCache;
	VkShaderModule			shaderVert[VK_SHADER_COUNT];
	VkShaderModule			shaderFrag[VK_SHADER_COUNT];
	VkDescriptorPool		descriptorPool;

	// live draw-recording state (set by the dispatched GL_* leaves)
	struct {
		float		mvp[16];			// push constant
		unsigned	stateBits;			// last GL_State
		int			cullType;			// last GL_Cull
		image_t		*image[2];			// bound texture per TMU
		int			multitexEnv;		// last GL_TexEnv on unit 1
	} draw;
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
// vk_memory.c -- device-memory helpers + per-frame vertex/index streaming
//
VkDeviceMemory	VK_AllocBufferMemory( VkBuffer buffer, VkMemoryPropertyFlags props, void **mapped );
qboolean	VK_CreateStreamingBuffers( void );
void		VK_DestroyStreamingBuffers( void );
void		VK_ResetStreaming( void );							// call at frame start
// append into the current frame's rings; return byte offsets (or qfalse on overflow)
qboolean	VK_StreamVertexes( const vkVertex_t *verts, int count, VkDeviceSize *outOffset );
qboolean	VK_StreamIndexes( const glIndex_t *indexes, int count, VkDeviceSize *outOffset );

//
// vk_image.c -- texture upload, samplers, descriptor sets
//
void		VK_CreateImage( image_t *image, const byte *pic, qboolean isLightmap );
void		VK_DeleteImages( void );
void		VK_TextureMode( const char *string );
qboolean	VK_InitImageSystem( void );							// descriptor pool, sampler cache
void		VK_ShutdownImageSystem( void );

//
// vk_pipeline.c -- descriptor/pipeline layouts, pipeline cache
//
qboolean	VK_InitPipelines( void );
void		VK_ShutdownPipelines( void );
VkPipeline	VK_GetPipeline( const vkPipelineKey_t *key );

//
// vk_backend.c
//
void		VKBE_Install( backend_t *b );		// fill the dispatch table with VK_* leaves
// (the dispatched GL-leaf equivalents VK_Set2D/VK_State/VK_Cull/VK_Bind/
//  VK_DrawElements/VK_BeginFrame/VK_EndFrame are declared in tr_local.h so the
//  shared backend files can call them without pulling in the Vulkan headers.)
void		VK_InstallInertGLProcs( void );		// point GL array/immediate qgl* at no-ops

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
