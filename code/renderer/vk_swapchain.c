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
// vk_swapchain.c -- swapchain, shared depth/stencil attachment and per-frame
// synchronization objects.  We use dynamic rendering (Vulkan 1.3 core), so there
// are no VkRenderPass / VkFramebuffer objects to manage.
//
#include "vk_local.h"
#include "vk_dlss.h"

/*
================
VK_ChooseSurfaceFormat

Prefer a plain UNORM B8G8R8A8 surface.  We deliberately avoid an sRGB surface:
Quake 3 already applies gamma/light-scaling on the CPU at texture-upload time,
so a linear UNORM swapchain reproduces the OpenGL output exactly.
================
*/
static void VK_ChooseSurfaceFormat( void ) {
	VkSurfaceFormatKHR	formats[64];
	uint32_t			count = 0, i;

	qvkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &count, NULL );
	if ( count > 64 ) {
		count = 64;
	}
	qvkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &count, formats );

	// default to the first reported pair
	vk.surfaceFormat = formats[0];

	for ( i = 0; i < count; i++ ) {
		if ( ( formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
			   formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ) &&
			 formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ) {
			vk.surfaceFormat = formats[i];
			break;
		}
	}
}

/*
================
VK_ChoosePresentMode

FIFO (always supported) == vsync on.  IMMEDIATE == vsync off.  r_swapInterval 0
means "no vsync"; anything else keeps FIFO.
================
*/
static void VK_ChoosePresentMode( void ) {
	VkPresentModeKHR	modes[8];
	uint32_t			count = 0, i;
	qboolean			wantVsync;
	qboolean			haveImmediate = qfalse;
	qboolean			haveMailbox = qfalse;

	wantVsync = ( r_swapInterval->integer != 0 );

	vk.presentMode = VK_PRESENT_MODE_FIFO_KHR;	// guaranteed available
	if ( wantVsync ) {
		return;
	}

	qvkGetPhysicalDeviceSurfacePresentModesKHR( vk.physicalDevice, vk.surface, &count, NULL );
	if ( count > 8 ) {
		count = 8;
	}
	qvkGetPhysicalDeviceSurfacePresentModesKHR( vk.physicalDevice, vk.surface, &count, modes );

	for ( i = 0; i < count; i++ ) {
		if ( modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR ) {
			haveImmediate = qtrue;
		} else if ( modes[i] == VK_PRESENT_MODE_MAILBOX_KHR ) {
			haveMailbox = qtrue;
		}
	}

	if ( haveImmediate ) {
		vk.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	} else if ( haveMailbox ) {
		vk.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
	}
}

/*
================
VK_ChooseDepthFormat
================
*/
static VkFormat VK_ChooseDepthFormat( void ) {
	VkFormat			candidates[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
	int					i;

	for ( i = 0; i < 2; i++ ) {
		VkFormatProperties props;
		qvkGetPhysicalDeviceFormatProperties( vk.physicalDevice, candidates[i], &props );
		if ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ) {
			return candidates[i];
		}
	}
	return VK_FORMAT_D24_UNORM_S8_UINT;
}

/*
================
VK_CreateDepthBuffer
================
*/
static qboolean VK_CreateDepthBuffer( void ) {
	VkImageCreateInfo		imageInfo;
	VkMemoryRequirements	memReq;
	VkMemoryAllocateInfo	allocInfo;
	VkImageViewCreateInfo	viewInfo;
	int						i;

	vk.depthFormat = VK_ChooseDepthFormat();

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		memset( &imageInfo, 0, sizeof( imageInfo ) );
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = vk.depthFormat;
		imageInfo.extent.width = vk.renderExtent.width;	// matches the scene render target (SSAA = 2x)
		imageInfo.extent.height = vk.renderExtent.height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VK_CHECK( qvkCreateImage( vk.device, &imageInfo, NULL, &vk.depthImage[i] ) );

		qvkGetImageMemoryRequirements( vk.device, vk.depthImage[i], &memReq );

		memset( &allocInfo, 0, sizeof( allocInfo ) );
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = VK_FindMemoryType( memReq.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
		VK_CHECK( qvkAllocateMemory( vk.device, &allocInfo, NULL, &vk.depthMemory[i] ) );
		VK_CHECK( qvkBindImageMemory( vk.device, vk.depthImage[i], vk.depthMemory[i], 0 ) );

		memset( &viewInfo, 0, sizeof( viewInfo ) );
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = vk.depthImage[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = vk.depthFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &vk.depthView[i] ) );
	}

	return qtrue;
}

/*
================
VK_CreateOffscreenTargets

For FXAA/SSAA the scene renders into a per-frame offscreen color image (sized to
renderExtent) instead of straight to the swapchain; VK_EndFrame resolves it.  Off
mode needs no offscreen image.  Per-frame-in-flight, like the depth buffer.
================
*/
static qboolean VK_CreateOffscreenTargets( void ) {
	VkImageCreateInfo		imageInfo;
	VkMemoryRequirements	memReq;
	VkMemoryAllocateInfo	allocInfo;
	VkImageViewCreateInfo	viewInfo;
	int						i;

	if ( vk.aaMode == VK_AA_OFF ) {
		return qtrue;
	}

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		memset( &imageInfo, 0, sizeof( imageInfo ) );
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = vk.surfaceFormat.format;
		imageInfo.extent.width = vk.renderExtent.width;
		imageInfo.extent.height = vk.renderExtent.height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |	// scene target
						  VK_IMAGE_USAGE_SAMPLED_BIT |			// FXAA post pass samples it
						  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;		// SSAA blits it down
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VK_CHECK( qvkCreateImage( vk.device, &imageInfo, NULL, &vk.offscreenImage[i] ) );

		qvkGetImageMemoryRequirements( vk.device, vk.offscreenImage[i], &memReq );

		memset( &allocInfo, 0, sizeof( allocInfo ) );
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = VK_FindMemoryType( memReq.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
		VK_CHECK( qvkAllocateMemory( vk.device, &allocInfo, NULL, &vk.offscreenMemory[i] ) );
		VK_CHECK( qvkBindImageMemory( vk.device, vk.offscreenImage[i], vk.offscreenMemory[i], 0 ) );

		memset( &viewInfo, 0, sizeof( viewInfo ) );
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = vk.offscreenImage[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = vk.surfaceFormat.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &vk.offscreenView[i] ) );
	}

	return qtrue;
}

/*
================
VK_DestroyOffscreenTargets
================
*/
static void VK_DestroyOffscreenTargets( void ) {
	int i;

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		if ( vk.offscreenView[i] ) {
			qvkDestroyImageView( vk.device, vk.offscreenView[i], NULL );
			vk.offscreenView[i] = VK_NULL_HANDLE;
		}
		if ( vk.offscreenImage[i] ) {
			qvkDestroyImage( vk.device, vk.offscreenImage[i], NULL );
			vk.offscreenImage[i] = VK_NULL_HANDLE;
		}
		if ( vk.offscreenMemory[i] ) {
			qvkFreeMemory( vk.device, vk.offscreenMemory[i], NULL );
			vk.offscreenMemory[i] = VK_NULL_HANDLE;
		}
	}
}

/*
================
VK_CreateSwapchain
================
*/
qboolean VK_CreateSwapchain( void ) {
	VkSurfaceCapabilitiesKHR	caps;
	VkSwapchainCreateInfoKHR	createInfo;
	uint32_t					queueFamilies[2];
	uint32_t					desiredImages;
	uint32_t					i;
	VkResult					res;

	res = qvkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physicalDevice, vk.surface, &caps );
	if ( res != VK_SUCCESS ) {
		ri.Printf( PRINT_ALL, "...vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %s\n", VK_ResultString( res ) );
		return qfalse;
	}

	VK_ChooseSurfaceFormat();
	VK_ChoosePresentMode();

	// resolve the swapchain extent
	if ( caps.currentExtent.width != 0xFFFFFFFF ) {
		vk.extent = caps.currentExtent;
	} else {
		vk.extent.width = glConfig.vidWidth;
		vk.extent.height = glConfig.vidHeight;
		if ( vk.extent.width < caps.minImageExtent.width )  vk.extent.width = caps.minImageExtent.width;
		if ( vk.extent.height < caps.minImageExtent.height ) vk.extent.height = caps.minImageExtent.height;
		if ( vk.extent.width > caps.maxImageExtent.width )  vk.extent.width = caps.maxImageExtent.width;
		if ( vk.extent.height > caps.maxImageExtent.height ) vk.extent.height = caps.maxImageExtent.height;
	}
	if ( vk.extent.width == 0 || vk.extent.height == 0 ) {
		// window minimized; nothing to create yet
		return qfalse;
	}

	// keep glConfig in sync with the real swapchain size
	glConfig.vidWidth = vk.extent.width;
	glConfig.vidHeight = vk.extent.height;

	// resolve the antialiasing mode (latched cvar; read once per swapchain build).
	// FXAA renders at display res into an offscreen image then runs a post pass;
	// SSAA renders the scene 2x larger (renderExtent) then downsamples with a blit.
	vk.aaMode = r_antialiasing ? r_antialiasing->integer : 0;
	if ( vk.aaMode < 0 || vk.aaMode > VK_AA_SSAA ) {
		vk.aaMode = VK_AA_OFF;
	}
	if ( vk.aaMode == VK_AA_SSAA ) {
		// integer supersample factor per axis; clamp down so the (factor x) offscreen
		// never exceeds the device's max 2D image dimension (8x is large at high res).
		int factor = VK_SSAA_FACTOR;
		uint32_t maxDim = vk.devProps.limits.maxImageDimension2D;
		while ( factor > 1 &&
			( (uint32_t)vk.extent.width * factor > maxDim || (uint32_t)vk.extent.height * factor > maxDim ) ) {
			factor--;
		}
		if ( factor < VK_SSAA_FACTOR ) {
			ri.Printf( PRINT_WARNING, "...SSAA clamped to %dx (device max image %u)\n", factor, maxDim );
		}
		vk.ssaaFactor = factor;
	} else {
		vk.ssaaFactor = 1;
	}
	vk.ssaaScale = (float)vk.ssaaFactor;
	vk.renderExtent.width  = vk.extent.width  * vk.ssaaFactor;
	vk.renderExtent.height = vk.extent.height * vk.ssaaFactor;

	// DLSS upscaling takes precedence over plain AA: render the scene into a
	// sub-display offscreen target (renderExtent) and upscale to the swapchain in
	// VK_EndFrame.  We reuse the offscreen plumbing; the resolve is the NGX neural
	// evaluate when the SDK is present, otherwise a linear blit (still a real win
	// since far fewer pixels are shaded).  See vk_dlss.c / DLSS_VULKAN_REFERENCE.md.
	vk.dlssMode = r_dlss ? r_dlss->integer : 0;
	if ( vk.dlssMode < VK_DLSS_OFF || vk.dlssMode > VK_DLSS_ULTRA_PERF ) {
		vk.dlssMode = VK_DLSS_OFF;
	}
	if ( vk.dlssMode != VK_DLSS_OFF ) {
		uint32_t rw, rh;
		if ( VK_DLSS_RenderResolution( vk.dlssMode, vk.extent.width, vk.extent.height, &rw, &rh ) ) {
			vk.aaMode = VK_AA_DLSS;
			vk.ssaaFactor = 1;
			vk.renderExtent.width  = rw;
			vk.renderExtent.height = rh;
			vk.ssaaScale = (float)rw / (float)vk.extent.width;	// < 1 (sub-display)
			VK_DLSS_Init();
			ri.Printf( PRINT_ALL, "...DLSS %s: rendering %ux%u -> %ux%u%s\n",
				VK_DLSS_ModeName( vk.dlssMode ), rw, rh, vk.extent.width, vk.extent.height,
				VK_DLSS_Available() ? " (NGX neural)" : " (linear upscale)" );
		} else {
			vk.dlssMode = VK_DLSS_OFF;
		}
	}

	desiredImages = caps.minImageCount + 1;
	if ( caps.maxImageCount > 0 && desiredImages > caps.maxImageCount ) {
		desiredImages = caps.maxImageCount;
	}
	if ( desiredImages > MAX_SWAPCHAIN_IMAGES ) {
		desiredImages = MAX_SWAPCHAIN_IMAGES;
	}

	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = vk.surface;
	createInfo.minImageCount = desiredImages;
	createInfo.imageFormat = vk.surfaceFormat.format;
	createInfo.imageColorSpace = vk.surfaceFormat.colorSpace;
	createInfo.imageExtent = vk.extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
							VK_IMAGE_USAGE_TRANSFER_SRC_BIT;	// TRANSFER_SRC for screenshot readback
	createInfo.preTransform = caps.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = vk.presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	queueFamilies[0] = vk.graphicsFamily;
	queueFamilies[1] = vk.presentFamily;
	if ( vk.graphicsFamily != vk.presentFamily ) {
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilies;
	} else {
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	res = qvkCreateSwapchainKHR( vk.device, &createInfo, NULL, &vk.swapchain );
	if ( res != VK_SUCCESS ) {
		ri.Printf( PRINT_ALL, "...vkCreateSwapchainKHR failed: %s\n", VK_ResultString( res ) );
		return qfalse;
	}

	vk.imageCount = 0;
	qvkGetSwapchainImagesKHR( vk.device, vk.swapchain, &vk.imageCount, NULL );
	if ( vk.imageCount > MAX_SWAPCHAIN_IMAGES ) {
		vk.imageCount = MAX_SWAPCHAIN_IMAGES;
	}
	qvkGetSwapchainImagesKHR( vk.device, vk.swapchain, &vk.imageCount, vk.swapchainImages );

	for ( i = 0; i < vk.imageCount; i++ ) {
		VkImageViewCreateInfo viewInfo;
		memset( &viewInfo, 0, sizeof( viewInfo ) );
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = vk.swapchainImages[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = vk.surfaceFormat.format;
		viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &vk.swapchainViews[i] ) );
	}

	if ( !VK_CreateDepthBuffer() ) {
		return qfalse;
	}
	if ( !VK_CreateOffscreenTargets() ) {
		return qfalse;
	}
	// re-point the FXAA sampler sets at the (re)created offscreen views.  No-op on the
	// first build (VK_InitPostProcess has not run yet); it updates them itself then.
	VK_UpdateOffscreenDescriptors();

	glConfig.isFullscreen = ( r_fullscreen->integer != 0 );
	vk.swapchainValid = qtrue;

	ri.Printf( PRINT_ALL, "...swapchain: %d x %d, %d images\n",
		vk.extent.width, vk.extent.height, vk.imageCount );
	return qtrue;
}

/*
================
VK_DestroySwapchain
================
*/
void VK_DestroySwapchain( void ) {
	uint32_t i;

	VK_DestroyOffscreenTargets();

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		if ( vk.depthView[i] ) {
			qvkDestroyImageView( vk.device, vk.depthView[i], NULL );
			vk.depthView[i] = VK_NULL_HANDLE;
		}
		if ( vk.depthImage[i] ) {
			qvkDestroyImage( vk.device, vk.depthImage[i], NULL );
			vk.depthImage[i] = VK_NULL_HANDLE;
		}
		if ( vk.depthMemory[i] ) {
			qvkFreeMemory( vk.device, vk.depthMemory[i], NULL );
			vk.depthMemory[i] = VK_NULL_HANDLE;
		}
	}

	for ( i = 0; i < vk.imageCount; i++ ) {
		if ( vk.swapchainViews[i] ) {
			qvkDestroyImageView( vk.device, vk.swapchainViews[i], NULL );
			vk.swapchainViews[i] = VK_NULL_HANDLE;
		}
	}
	vk.imageCount = 0;

	if ( vk.swapchain ) {
		qvkDestroySwapchainKHR( vk.device, vk.swapchain, NULL );
		vk.swapchain = VK_NULL_HANDLE;
	}
	vk.swapchainValid = qfalse;
}

/*
================
VK_RecreateSwapchain

On resize / VK_ERROR_OUT_OF_DATE_KHR: drain the GPU, rebuild the swapchain and
depth buffer.  Per-frame command buffers and sync objects are preserved.
================
*/
qboolean VK_RecreateSwapchain( void ) {
	if ( vk.device && qvkDeviceWaitIdle ) {
		qvkDeviceWaitIdle( vk.device );
	}
	VK_DestroySwapchain();
	return VK_CreateSwapchain();
}

/*
================
VK_CreateFrameResources

Command pool + one primary command buffer, plus image-acquired / render-complete
semaphores and an in-flight fence, for each of VK_NUM_FRAMES frames in flight.
================
*/
qboolean VK_CreateFrameResources( void ) {
	VkCommandPoolCreateInfo		poolInfo;
	VkCommandBufferAllocateInfo	allocInfo;
	VkSemaphoreCreateInfo		semInfo;
	VkFenceCreateInfo			fenceInfo;
	int							i;

	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = vk.graphicsFamily;
	VK_CHECK( qvkCreateCommandPool( vk.device, &poolInfo, NULL, &vk.commandPool ) );

	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = vk.commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = VK_NUM_FRAMES;
	VK_CHECK( qvkAllocateCommandBuffers( vk.device, &allocInfo, vk.commandBuffers ) );

	memset( &semInfo, 0, sizeof( semInfo ) );
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	memset( &fenceInfo, 0, sizeof( fenceInfo ) );
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;	// so the first wait returns immediately

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		VK_CHECK( qvkCreateSemaphore( vk.device, &semInfo, NULL, &vk.imageAcquired[i] ) );
		VK_CHECK( qvkCreateFence( vk.device, &fenceInfo, NULL, &vk.frameFence[i] ) );
	}

	// one present-wait semaphore per swapchain image (see vk_local.h); allocate the
	// maximum so the set survives a swapchain rebuild with a different image count.
	for ( i = 0; i < MAX_SWAPCHAIN_IMAGES; i++ ) {
		VK_CHECK( qvkCreateSemaphore( vk.device, &semInfo, NULL, &vk.renderComplete[i] ) );
	}

	vk.frameIndex = 0;
	return qtrue;
}

/*
================
VK_DestroyFrameResources
================
*/
void VK_DestroyFrameResources( void ) {
	int i;

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		if ( vk.imageAcquired[i] ) {
			qvkDestroySemaphore( vk.device, vk.imageAcquired[i], NULL );
			vk.imageAcquired[i] = VK_NULL_HANDLE;
		}
		if ( vk.frameFence[i] ) {
			qvkDestroyFence( vk.device, vk.frameFence[i], NULL );
			vk.frameFence[i] = VK_NULL_HANDLE;
		}
	}

	for ( i = 0; i < MAX_SWAPCHAIN_IMAGES; i++ ) {
		if ( vk.renderComplete[i] ) {
			qvkDestroySemaphore( vk.device, vk.renderComplete[i], NULL );
			vk.renderComplete[i] = VK_NULL_HANDLE;
		}
	}

	if ( vk.commandPool ) {
		qvkDestroyCommandPool( vk.device, vk.commandPool, NULL );
		vk.commandPool = VK_NULL_HANDLE;
	}
}
