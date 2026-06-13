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
// vk_backend.c -- the Vulkan frame loop and the GPU-leaf functions the shared
// tr_backend.c / tr_shade.c draw path dispatches to under r_renderapi 1.
//
// The command list is drained by the SAME RB_ExecuteRenderCommands as OpenGL;
// the RB_* handlers and the tr_shade.c stage iterators run unchanged and route
// their leaf operations (GL_State / GL_Bind / R_DrawElements / RB_SetGL2D /
// frame begin+present) here.  This keeps the parity-critical front-of-backend
// (deforms, rgbGen/alphaGen, tcGen/tcMod, fog, dlight) as a single shared
// implementation.
//
#include "vk_local.h"

#define VK_TIMEOUT_NS	( (uint64_t)1000000000 * 5 )	// 5s acquire/fence timeout

/*
================
VK_ImageBarrier
================
*/
static void VK_ImageBarrier( VkImage image, VkImageAspectFlags aspect,
	VkImageLayout oldLayout, VkImageLayout newLayout,
	VkAccessFlags srcAccess, VkAccessFlags dstAccess,
	VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage )
{
	VkImageMemoryBarrier barrier;

	memset( &barrier, 0, sizeof( barrier ) );
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	qvkCmdPipelineBarrier( vk.cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier );
}

/*
================
VK_BeginFrame

Acquire the next swapchain image and open the frame's command buffer inside a
dynamic-rendering pass that clears color + depth.  Dispatched from RB_DrawBuffer.
================
*/
void VK_BeginFrame( void ) {
	VkCommandBufferBeginInfo	beginInfo;
	VkRenderingAttachmentInfo	colorAttachment;
	VkRenderingAttachmentInfo	depthAttachment;
	VkRenderingInfo				renderingInfo;
	VkViewport					viewport;
	VkRect2D					scissor;
	VkResult					res;
	int							frame = vk.frameIndex;
	VkImage						colorImage;		// scene color target: offscreen (FXAA/SSAA) or swapchain (Off)
	VkImageView					colorView;

	if ( !vk.initialized || vk.frameStarted ) {
		return;
	}

	// vsync toggle: r_swapInterval change -> rebuild the swapchain with the new
	// present mode (no full vid_restart needed)
	if ( r_swapInterval->modified ) {
		r_swapInterval->modified = qfalse;
		vk.swapchainValid = qfalse;
	}

	if ( !vk.swapchainValid ) {
		if ( !VK_RecreateSwapchain() ) {
			return;	// minimized / not presentable
		}
	}

	// make sure any textures created since the last frame are on the GPU
	VK_FlushUploads();

	qvkWaitForFences( vk.device, 1, &vk.frameFence[frame], VK_TRUE, VK_TIMEOUT_NS );

	res = qvkAcquireNextImageKHR( vk.device, vk.swapchain, VK_TIMEOUT_NS,
		vk.imageAcquired[frame], VK_NULL_HANDLE, &vk.swapchainIndex );
	if ( res == VK_ERROR_OUT_OF_DATE_KHR ) {
		VK_RecreateSwapchain();
		return;
	}
	if ( res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR ) {
		ri.Printf( PRINT_ALL, "vkAcquireNextImageKHR: %s\n", VK_ResultString( res ) );
		return;
	}

	// the GPU is done with this frame slot: its streaming rings are free to reuse
	VK_ResetStreaming();

	vk.cmd = vk.commandBuffers[frame];
	qvkResetCommandBuffer( vk.cmd, 0 );

	memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	qvkBeginCommandBuffer( vk.cmd, &beginInfo );

	// the scene pass draws into renderExtent at ssaaScale; VK_Set2D may flip these to
	// the native swapchain size mid-frame for the DLSS 2D pass (see VK_Set2D).
	vk.curExtent = vk.renderExtent;
	vk.curScale  = vk.ssaaScale;
	vk.on2DTarget = qfalse;
	vk.lightUboFilled = qfalse;		// Blinn-Phong light UBO: refilled per view (see VK_FillLightUbo)
	vk.lightViewSlot = -1;			// next lit view takes ring slot 0

	// scene color target: the offscreen image for FXAA/SSAA (resolved to the swapchain
	// in VK_EndFrame), or the swapchain itself for Off.  Both use a negative-height
	// viewport and are sized to renderExtent (= swapchain extent, or 2x under SSAA).
	if ( VK_USES_OFFSCREEN() ) {
		colorImage = vk.offscreenImage[frame];
		colorView  = vk.offscreenView[frame];
	} else {
		colorImage = vk.swapchainImages[vk.swapchainIndex];
		colorView  = vk.swapchainViews[vk.swapchainIndex];
	}

	VK_ImageBarrier( colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );

	VK_ImageBarrier( vk.depthImage[frame], VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT );

	memset( &colorAttachment, 0, sizeof( colorAttachment ) );
	colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colorAttachment.imageView = colorView;
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.clearValue.color.float32[0] = 0.0f;
	colorAttachment.clearValue.color.float32[1] = 0.0f;
	colorAttachment.clearValue.color.float32[2] = 0.0f;
	colorAttachment.clearValue.color.float32[3] = 1.0f;

	memset( &depthAttachment, 0, sizeof( depthAttachment ) );
	depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	depthAttachment.imageView = vk.depthView[frame];
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.clearValue.depthStencil.depth = 1.0f;
	depthAttachment.clearValue.depthStencil.stencil = 0;

	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = vk.renderExtent;	// SSAA renders 2x larger
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;

	qvkCmdBeginRendering( vk.cmd, &renderingInfo );

	memset( &viewport, 0, sizeof( viewport ) );
	viewport.x = 0.0f;
	viewport.y = (float)vk.renderExtent.height;		// negative-height viewport (GL-compatible Y)
	viewport.width = (float)vk.renderExtent.width;
	viewport.height = -(float)vk.renderExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vk.draw.viewport = viewport;
	qvkCmdSetViewport( vk.cmd, 0, 1, &viewport );

	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent = vk.renderExtent;
	qvkCmdSetScissor( vk.cmd, 0, 1, &scissor );

	// reset the per-draw recording state
	memset( &vk.draw, 0, sizeof( vk.draw ) );
	glState.glStateBits = 0;
	glState.faceCulling = -1;

	vk.frameStarted = qtrue;
}

/*
================
VK_RequestScreenshot

bk screenshot leaf: remember the request; the readback happens in VK_EndFrame
once the frame has been rendered and submitted.
================
*/
void VK_RequestScreenshot( const char *name, qboolean jpeg ) {
	Q_strncpyz( vk.screenshotName, name, sizeof( vk.screenshotName ) );
	vk.screenshotJpeg = jpeg;
	vk.screenshotPending = qtrue;
}

/*
================
VK_RecordScreenshotCopy

Record (into the frame's command buffer) a copy of the rendered swapchain image
into a host-visible buffer, leaving the image in PRESENT layout.
================
*/
static void VK_RecordScreenshotCopy( VkImageLayout from ) {
	VkBufferCreateInfo	bufInfo;
	VkBufferImageCopy	region;
	VkDeviceSize		size = (VkDeviceSize)vk.extent.width * vk.extent.height * 4;
	VkAccessFlags		srcAccess;
	VkPipelineStageFlags srcStage;

	memset( &bufInfo, 0, sizeof( bufInfo ) );
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size = size;
	bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK( qvkCreateBuffer( vk.device, &bufInfo, NULL, &vk.screenshotBuffer ) );
	vk.screenshotMemory = VK_AllocBufferMemory( vk.screenshotBuffer,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL );

	// the swapchain holds the final image in either COLOR_ATTACHMENT (Off/FXAA) or
	// TRANSFER_DST (SSAA blit) layout; match the source access/stage accordingly
	if ( from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ) {
		srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else {
		srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}

	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		srcAccess, VK_ACCESS_TRANSFER_READ_BIT,
		srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT );

	memset( &region, 0, sizeof( region ) );
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = vk.extent.width;
	region.imageExtent.height = vk.extent.height;
	region.imageExtent.depth = 1;
	qvkCmdCopyImageToBuffer( vk.cmd, vk.swapchainImages[vk.swapchainIndex],
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.screenshotBuffer, 1, &region );

	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_ACCESS_TRANSFER_READ_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT );
}

/*
================
VK_WriteScreenshot

After the copy has completed, encode the host buffer as TGA or JPEG and write it.
The swapchain image is top-to-bottom BGRA (or RGBA); convert as needed.
================
*/
static void VK_ReleaseScreenshotBuffer( void ) {
	if ( vk.screenshotMemory ) {
		qvkFreeMemory( vk.device, vk.screenshotMemory, NULL );
		vk.screenshotMemory = VK_NULL_HANDLE;
	}
	if ( vk.screenshotBuffer ) {
		qvkDestroyBuffer( vk.device, vk.screenshotBuffer, NULL );
		vk.screenshotBuffer = VK_NULL_HANDLE;
	}
}

static void VK_WriteScreenshot( void ) {
	byte		*src = NULL;
	int			w = vk.extent.width;
	int			h = vk.extent.height;
	qboolean	bgra = ( vk.surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM );
	int			x, y;

	if ( qvkMapMemory( vk.device, vk.screenshotMemory, 0, VK_WHOLE_SIZE, 0, (void **)&src ) != VK_SUCCESS ) {
		ri.Printf( PRINT_WARNING, "VK screenshot: map failed\n" );
		VK_ReleaseScreenshotBuffer();
		return;
	}

	if ( vk.screenshotJpeg ) {
		// SaveJPG wants a GL-convention (bottom-to-top) RGBA buffer
		byte *rgba = ri.Hunk_AllocateTempMemory( w * h * 4 );
		for ( y = 0; y < h; y++ ) {
			const byte *s = src + (size_t)( h - 1 - y ) * w * 4;
			byte *d = rgba + (size_t)y * w * 4;
			for ( x = 0; x < w; x++, s += 4, d += 4 ) {
				if ( bgra ) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
				else        { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
				d[3] = 255;
			}
		}
		ri.FS_WriteFile( vk.screenshotName, rgba, 1 );	// create the path
		SaveJPG( vk.screenshotName, 95, w, h, rgba );
		ri.Hunk_FreeTempMemory( rgba );
	} else {
		// uncompressed TGA, top-left origin (matches our top-to-bottom buffer), BGR
		byte *tga = ri.Hunk_AllocateTempMemory( w * h * 3 + 18 );
		byte *d = tga + 18;
		const byte *s = src;
		Com_Memset( tga, 0, 18 );
		tga[2] = 2;
		tga[12] = w & 255; tga[13] = ( w >> 8 ) & 255;
		tga[14] = h & 255; tga[15] = ( h >> 8 ) & 255;
		tga[16] = 24;
		tga[17] = 0x20;		// top-left origin
		for ( y = 0; y < h; y++ ) {
			for ( x = 0; x < w; x++, s += 4, d += 3 ) {
				if ( bgra ) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
				else        { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
			}
		}
		ri.FS_WriteFile( vk.screenshotName, tga, w * h * 3 + 18 );
		ri.Hunk_FreeTempMemory( tga );
	}

	qvkUnmapMemory( vk.device, vk.screenshotMemory );
	VK_ReleaseScreenshotBuffer();
	ri.Printf( PRINT_ALL, "Wrote %s\n", vk.screenshotName );
}

/*
================
VK_ResolveFXAA

FXAA: the scene was rendered into the offscreen color image.  Run a fullscreen
pass that samples it through the FXAA shader, writing into the swapchain.  Leaves
the swapchain in COLOR_ATTACHMENT_OPTIMAL.
================
*/
static void VK_ResolveFXAA( void ) {
	VkRenderingAttachmentInfo	colorAttachment;
	VkRenderingInfo				renderingInfo;
	VkViewport					vp;
	VkRect2D					sc;
	float						invRes[2];
	int							frame = vk.frameIndex;

	// offscreen scene color: COLOR_ATTACHMENT -> shader read
	VK_ImageBarrier( vk.offscreenImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );

	// swapchain: UNDEFINED -> color attachment (we overwrite every pixel)
	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );

	memset( &colorAttachment, 0, sizeof( colorAttachment ) );
	colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colorAttachment.imageView = vk.swapchainViews[vk.swapchainIndex];
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = vk.extent;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;

	qvkCmdBeginRendering( vk.cmd, &renderingInfo );

	// POSITIVE-height viewport: fullscreen.vert maps uv 0..1 top-left, matching the
	// offscreen storage, so a normal viewport gives a 1:1 sample->screen copy.
	memset( &vp, 0, sizeof( vp ) );
	vp.x = 0.0f;
	vp.y = 0.0f;
	vp.width = (float)vk.extent.width;
	vp.height = (float)vk.extent.height;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	qvkCmdSetViewport( vk.cmd, 0, 1, &vp );

	sc.offset.x = 0;
	sc.offset.y = 0;
	sc.extent = vk.extent;
	qvkCmdSetScissor( vk.cmd, 0, 1, &sc );

	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeFXAA );
	qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.postLayout,
		0, 1, &vk.offscreenDesc[frame], 0, NULL );
	invRes[0] = 1.0f / (float)vk.renderExtent.width;
	invRes[1] = 1.0f / (float)vk.renderExtent.height;
	qvkCmdPushConstants( vk.cmd, vk.postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( invRes ), invRes );
	qvkCmdDraw( vk.cmd, 3, 1, 0, 0 );

	qvkCmdEndRendering( vk.cmd );
}

/*
================
VK_ResolveSSAA

SSAA: the scene was rendered into the (factor x) larger offscreen image.  Run a
fullscreen pass that box-averages each factor x factor source block into one
display pixel (a true N-times downsample).  Leaves the swapchain in
COLOR_ATTACHMENT_OPTIMAL (same as FXAA).
================
*/
static void VK_ResolveSSAA( void ) {
	VkRenderingAttachmentInfo	colorAttachment;
	VkRenderingInfo				renderingInfo;
	VkViewport					vp;
	VkRect2D					sc;
	struct { float invSrcRes[2]; int factor; } pc;
	int							frame = vk.frameIndex;

	// offscreen scene color: COLOR_ATTACHMENT -> shader read
	VK_ImageBarrier( vk.offscreenImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );

	// swapchain: UNDEFINED -> color attachment (we overwrite every pixel)
	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );

	memset( &colorAttachment, 0, sizeof( colorAttachment ) );
	colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colorAttachment.imageView = vk.swapchainViews[vk.swapchainIndex];
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = vk.extent;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;

	qvkCmdBeginRendering( vk.cmd, &renderingInfo );

	// POSITIVE-height viewport: gl_FragCoord is the display pixel index, mapped to its
	// factor x factor source block in the downsample shader.
	memset( &vp, 0, sizeof( vp ) );
	vp.x = 0.0f;
	vp.y = 0.0f;
	vp.width = (float)vk.extent.width;
	vp.height = (float)vk.extent.height;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	qvkCmdSetViewport( vk.cmd, 0, 1, &vp );

	sc.offset.x = 0;
	sc.offset.y = 0;
	sc.extent = vk.extent;
	qvkCmdSetScissor( vk.cmd, 0, 1, &sc );

	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeDownsample );
	qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.postLayout,
		0, 1, &vk.offscreenDesc[frame], 0, NULL );
	pc.invSrcRes[0] = 1.0f / (float)vk.renderExtent.width;
	pc.invSrcRes[1] = 1.0f / (float)vk.renderExtent.height;
	pc.factor = vk.ssaaFactor;
	qvkCmdPushConstants( vk.cmd, vk.postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( pc ), &pc );
	qvkCmdDraw( vk.cmd, 3, 1, 0, 0 );

	qvkCmdEndRendering( vk.cmd );
}

/*
================
VK_SwapchainToPresent

Transition the swapchain image to PRESENT from whatever layout the resolve left it
in (COLOR_ATTACHMENT for Off/FXAA, TRANSFER_DST for SSAA).
================
*/
static void VK_SwapchainToPresent( VkImageLayout from ) {
	VkAccessFlags			srcAccess;
	VkPipelineStageFlags	srcStage;

	if ( from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ) {
		srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else {
		srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}
	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		from, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		srcAccess, 0, srcStage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT );
}

/*
================
VK_ResolveFrameMultisampling

Temporal accumulation.  The scene for this frame is in the offscreen color image.
Add it into the float16 running sum (msAccumImage); once every msFrames frames,
resolve the sum (x 1/msFrames) into msHeldImage.  The held image is copied to the
swapchain on EVERY frame, so the displayed picture is the average of msFrames
consecutive frames and refreshes fps/msFrames times a second.  This keeps the
acquire/present cycle untouched -- only the swapchain CONTENTS update every msFrames
frames.  Leaves the swapchain in TRANSFER_DST (handled by VK_SwapchainToPresent).

msAccumImage/msHeldImage are single-instance (the running sum is serial across
frames); correctness across the two frames-in-flight comes from these image barriers
plus same-queue submission order.
================
*/
static void VK_ResolveFrameMultisampling( void ) {
	VkRenderingAttachmentInfo	colorAttachment;
	VkRenderingInfo				renderingInfo;
	VkViewport					vp;
	VkRect2D					sc;
	float						scale;
	int							frame = vk.frameIndex;
	qboolean					firstOfWindow = ( vk.msCounter == 0 );
	qboolean					lastOfWindow  = ( vk.msCounter == vk.msFrames - 1 );

	// offscreen scene color: COLOR_ATTACHMENT -> shader read (sampled by the accumulate)
	VK_ImageBarrier( vk.offscreenImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );

	// running sum -> color attachment.  First frame of a window CLEARs it (old contents
	// discarded); the rest LOAD and add.  The wide src scope orders this after BOTH the
	// previous frame's accumulate write and the previous window's resolve read (the same
	// single image is touched by consecutive in-flight frames).
	VK_ImageBarrier( vk.msAccumImage, VK_IMAGE_ASPECT_COLOR_BIT,
		firstOfWindow ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );

	memset( &colorAttachment, 0, sizeof( colorAttachment ) );
	colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colorAttachment.imageView = vk.msAccumView;
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = firstOfWindow ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.clearValue.color.float32[0] = 0.0f;
	colorAttachment.clearValue.color.float32[1] = 0.0f;
	colorAttachment.clearValue.color.float32[2] = 0.0f;
	colorAttachment.clearValue.color.float32[3] = 0.0f;

	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = vk.extent;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;

	// POSITIVE-height viewport (same as the FXAA resolve): a 1:1 sample->store copy.
	memset( &vp, 0, sizeof( vp ) );
	vp.x = 0.0f;
	vp.y = 0.0f;
	vp.width = (float)vk.extent.width;
	vp.height = (float)vk.extent.height;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	sc.offset.x = 0;
	sc.offset.y = 0;
	sc.extent = vk.extent;

	qvkCmdBeginRendering( vk.cmd, &renderingInfo );
	qvkCmdSetViewport( vk.cmd, 0, 1, &vp );
	qvkCmdSetScissor( vk.cmd, 0, 1, &sc );
	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeMSAccum );
	qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.postLayout,
		0, 1, &vk.offscreenDesc[frame], 0, NULL );
	scale = 1.0f;
	qvkCmdPushConstants( vk.cmd, vk.postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( scale ), &scale );
	qvkCmdDraw( vk.cmd, 3, 1, 0, 0 );
	qvkCmdEndRendering( vk.cmd );

	// resolve (last frame of a window only): held = sum / msFrames
	if ( lastOfWindow ) {
		VK_ImageBarrier( vk.msAccumImage, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );

		// held -> color attachment (DONT_CARE: every pixel overwritten).  src=TRANSFER
		// orders the write after the previous frame's copy that read the held image.
		VK_ImageBarrier( vk.msHeldImage, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );

		colorAttachment.imageView = vk.msHeldView;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

		qvkCmdBeginRendering( vk.cmd, &renderingInfo );
		qvkCmdSetViewport( vk.cmd, 0, 1, &vp );
		qvkCmdSetScissor( vk.cmd, 0, 1, &sc );
		qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeMSResolve );
		qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.postLayout,
			0, 1, &vk.msAccumDesc, 0, NULL );
		scale = 1.0f / (float)vk.msFrames;
		qvkCmdPushConstants( vk.cmd, vk.postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( scale ), &scale );
		qvkCmdDraw( vk.cmd, 3, 1, 0, 0 );
		qvkCmdEndRendering( vk.cmd );

		vk.msHeldValid = qtrue;
	}

	// copy the held frame to the swapchain (every frame)
	if ( !vk.msHeldValid ) {
		// no resolved frame yet (first partial window): clear the swapchain to black
		VkClearColorValue		black;
		VkImageSubresourceRange	range;

		VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

		memset( &black, 0, sizeof( black ) );
		memset( &range, 0, sizeof( range ) );
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;
		qvkCmdClearColorImage( vk.cmd, vk.swapchainImages[vk.swapchainIndex],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range );
	} else {
		VkImageBlit	region;

		// held -> transfer src.  Just-resolved frames leave it in COLOR_ATTACHMENT; the
		// held (between-resolve) frames leave it in TRANSFER_SRC from the previous copy.
		VK_ImageBarrier( vk.msHeldImage, VK_IMAGE_ASPECT_COLOR_BIT,
			lastOfWindow ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			lastOfWindow ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_TRANSFER_READ_BIT,
			lastOfWindow ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT );

		VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

		memset( &region, 0, sizeof( region ) );
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.layerCount = 1;
		region.srcOffsets[1].x = vk.extent.width;
		region.srcOffsets[1].y = vk.extent.height;
		region.srcOffsets[1].z = 1;
		region.dstOffsets[1].x = vk.extent.width;
		region.dstOffsets[1].y = vk.extent.height;
		region.dstOffsets[1].z = 1;
		qvkCmdBlitImage( vk.cmd,
			vk.msHeldImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &region, VK_FILTER_NEAREST );
	}
}

/*
================
VK_EndFrame

Close the pass, submit and present.  Dispatched from RB_SwapBuffers.
================
*/
void VK_EndFrame( void ) {
	VkSubmitInfo			submitInfo;
	VkPresentInfoKHR		presentInfo;
	VkPipelineStageFlags	waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkResult				res;
	int						frame = vk.frameIndex;
	VkImageLayout			swapLayout;		// layout the swapchain is left in after the resolve

	if ( !vk.frameStarted ) {
		return;
	}

	qvkCmdEndRendering( vk.cmd );		// ends the scene pass, or the DLSS native-res 2D pass

	// resolve the offscreen scene color into the swapchain (FXAA = shader pass,
	// SSAA = downsampling blit); Off rendered straight into the swapchain already.
	if ( vk.msFrames >= 2 ) {
		// frame multisampling owns the present: accumulate this frame and copy the held
		// (averaged) frame to the swapchain.  Leaves the swapchain in TRANSFER_DST.
		VK_ResolveFrameMultisampling();
		swapLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	} else {
		if ( vk.on2DTarget ) {
			// DLSS: the scene was already upscaled to the swapchain in VK_Set2D and the 2D
			// overlay was drawn straight onto it at native res -- nothing left to resolve.
		} else if ( vk.aaMode == VK_AA_FXAA ) {
			VK_ResolveFXAA();
		} else if ( vk.aaMode == VK_AA_SSAA ) {
			VK_ResolveSSAA();
		} else if ( vk.aaMode == VK_AA_DLSS ) {
			// DLSS frame that drew no 2D (rare): upscale the offscreen now.  The fullscreen
			// FXAA pass samples it through a linear sampler, upscaling to the swapchain.
			// UPGRADE PATH: when an NGX feature is live and a render-res motion-vector
			// buffer is produced, call VK_DLSS_Evaluate(...) here instead (see vk_dlss.c).
			VK_ResolveFXAA();
		}
		// all resolve paths (and Off) leave the swapchain in COLOR_ATTACHMENT_OPTIMAL
		swapLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	if ( vk.screenshotPending ) {
		VK_RecordScreenshotCopy( swapLayout );	// copies the image and transitions it to PRESENT
	} else {
		VK_SwapchainToPresent( swapLayout );
	}

	qvkEndCommandBuffer( vk.cmd );

	qvkResetFences( vk.device, 1, &vk.frameFence[frame] );

	memset( &submitInfo, 0, sizeof( submitInfo ) );
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &vk.imageAcquired[frame];
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &vk.cmd;
	submitInfo.signalSemaphoreCount = 1;
	// present-wait semaphore is keyed to the swapchain image, not the frame slot
	submitInfo.pSignalSemaphores = &vk.renderComplete[vk.swapchainIndex];
	VK_CHECK( qvkQueueSubmit( vk.graphicsQueue, 1, &submitInfo, vk.frameFence[frame] ) );

	memset( &presentInfo, 0, sizeof( presentInfo ) );
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &vk.renderComplete[vk.swapchainIndex];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &vk.swapchain;
	presentInfo.pImageIndices = &vk.swapchainIndex;

	res = qvkQueuePresentKHR( vk.presentQueue, &presentInfo );
	if ( res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR ) {
		VK_RecreateSwapchain();
	} else if ( res != VK_SUCCESS ) {
		ri.Printf( PRINT_ALL, "vkQueuePresentKHR: %s\n", VK_ResultString( res ) );
	}

	// finish a pending screenshot now that the copy has been submitted
	if ( vk.screenshotPending ) {
		// only read the copy buffer once the GPU has actually finished it
		if ( qvkWaitForFences( vk.device, 1, &vk.frameFence[frame], VK_TRUE, VK_TIMEOUT_NS ) == VK_SUCCESS ) {
			VK_WriteScreenshot();
		} else {
			ri.Printf( PRINT_WARNING, "VK screenshot: fence wait failed\n" );
			VK_ReleaseScreenshotBuffer();
		}
		vk.screenshotPending = qfalse;
	}

	vk.frameStarted = qfalse;
	vk.frameIndex = ( frame + 1 ) % VK_NUM_FRAMES;
	if ( vk.msFrames >= 2 ) {
		vk.msCounter = ( vk.msCounter + 1 ) % vk.msFrames;	// advance the accumulation window
	}
}

//==========================================================================
//
// dispatched GL-leaf equivalents
//
//==========================================================================

/*
================
VK_Begin2DPass

DLSS only: after the sub-display 3D scene has been upscaled into the swapchain,
open a fresh native-resolution pass that draws the 2D overlay (HUD/console/menu)
straight onto the swapchain.  Colour is LOAD'ed (keep the upscaled scene); the
shared depth buffer (sized to max(renderExtent,extent)) is cleared so any 3D
models drawn during the 2D phase (e.g. the menu player preview) depth-test
correctly at native resolution.
================
*/
static void VK_Begin2DPass( void ) {
	VkRenderingAttachmentInfo	colorAttachment;
	VkRenderingAttachmentInfo	depthAttachment;
	VkRenderingInfo				renderingInfo;

	memset( &colorAttachment, 0, sizeof( colorAttachment ) );
	colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colorAttachment.imageView = vk.swapchainViews[vk.swapchainIndex];
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;	// keep the resolved 3D scene
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	memset( &depthAttachment, 0, sizeof( depthAttachment ) );
	depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	depthAttachment.imageView = vk.depthView[vk.frameIndex];
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.clearValue.depthStencil.depth = 1.0f;
	depthAttachment.clearValue.depthStencil.stencil = 0;

	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = vk.extent;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;

	qvkCmdBeginRendering( vk.cmd, &renderingInfo );
}

/*
================
VK_Set2D

RB_SetGL2D leaf: build the orthographic MVP (screen pixels -> Vulkan clip) and a
full-screen viewport/scissor.  Vulkan's clip space already has +Y downward, so
screen (0,0) maps to the top-left with no extra flip.
================
*/
void VK_Set2D( void ) {
	VkViewport	viewport;
	VkRect2D	scissor;
	float		w = (float)glConfig.vidWidth;
	float		h = (float)glConfig.vidHeight;

	// GL-style ortho (matches qglOrtho(0,w,h,0,0,1)): x [0,w]->[-1,1],
	// y [0,h]->[+1,-1] (GL y-up), z passthrough.  Combined with the negative-height
	// viewport below this lands screen (0,0) at the framebuffer top-left and keeps
	// GL's CCW winding (so 2D and 3D share one front face).
	Com_Memset( vk.draw.mvp, 0, sizeof( vk.draw.mvp ) );
	vk.draw.mvp[0]  =  2.0f / w;
	vk.draw.mvp[5]  = -2.0f / h;
	vk.draw.mvp[10] =  1.0f;
	vk.draw.mvp[12] = -1.0f;
	vk.draw.mvp[13] =  1.0f;
	vk.draw.mvp[15] =  1.0f;

	vk.draw.stateBits = GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
	vk.draw.cullType = CT_TWO_SIDED;
	vk.draw.clipPlane[0] = vk.draw.clipPlane[1] = vk.draw.clipPlane[2] = vk.draw.clipPlane[3] = 0.0f;	// 2D never clips

	if ( vk.frameStarted ) {
		// DLSS: the 3D scene was rendered into the sub-display offscreen.  On the FIRST
		// 2D draw of the frame, resolve/upscale it onto the swapchain and switch to
		// drawing the 2D overlay directly at native resolution (crisp text), instead of
		// drawing 2D into the low-res offscreen and upscaling it with the scene.
		if ( vk.aaMode == VK_AA_DLSS && !vk.on2DTarget ) {
			qvkCmdEndRendering( vk.cmd );		// end the 3D offscreen pass
			VK_ResolveFXAA();					// offscreen(renderExtent) -> swapchain(extent)
			VK_Begin2DPass();					// native-res swapchain pass for the 2D
			vk.on2DTarget = qtrue;
			vk.curExtent = vk.extent;
			vk.curScale = 1.0f;
		}

		// pixel rects scale by the current target's factor (SSAA > 1, DLSS 2D = 1.0) so
		// the HUD/2D fills the target; the ortho matrix above stays res-independent.
		{
			float s = vk.curScale;
			memset( &viewport, 0, sizeof( viewport ) );
			viewport.x = 0.0f;
			viewport.y = h * s;		// negative-height viewport (flip Y in the viewport transform)
			viewport.width = w * s;
			viewport.height = -h * s;
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vk.draw.viewport = viewport;
			qvkCmdSetViewport( vk.cmd, 0, 1, &viewport );

			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent = vk.curExtent;
			qvkCmdSetScissor( vk.cmd, 0, 1, &scissor );
		}
	}
}

/*
================
VK_Mat4Mul

out = a * b, all column-major 4x4 (OpenGL convention, as Q3 stores its matrices).
================
*/
static void VK_Mat4Mul( const float *a, const float *b, float *out ) {
	int c, r, k;
	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			float sum = 0.0f;
			for ( k = 0; k < 4; k++ ) {
				sum += a[k * 4 + r] * b[c * 4 + k];
			}
			out[c * 4 + r] = sum;
		}
	}
}

/*
================
VK_SetViewport

SetViewportAndScissor leaf: store the 3D projection and set the viewport/scissor.
Q3's viewport is bottom-left origin; convert to Vulkan's top-left framebuffer.
================
*/
void VK_SetViewport( void ) {
	VkViewport	vp;
	VkRect2D	sc;
	int			x = backEnd.viewParms.viewportX;
	int			w = backEnd.viewParms.viewportWidth;
	int			h = backEnd.viewParms.viewportHeight;
	int			yTop = glConfig.vidHeight - backEnd.viewParms.viewportY - h;	// top-left origin

	Com_Memcpy( vk.draw.projection, backEnd.viewParms.projectionMatrix, sizeof( vk.draw.projection ) );

	if ( !vk.frameStarted ) {
		return;
	}

	// scale pixel rects by the current target's factor (SSAA > 1, DLSS 2D = 1.0); the
	// projection above is resolution-independent and is NOT scaled.
	{
		float s = vk.curScale;
		memset( &vp, 0, sizeof( vp ) );
		vp.x = (float)x * s;
		vp.y = (float)( yTop + h ) * s;		// negative-height viewport (GL-compatible Y)
		vp.width = (float)w * s;
		vp.height = -(float)h * s;
		vp.minDepth = 0.0f;
		vp.maxDepth = 1.0f;
		vk.draw.viewport = vp;			// remembered so qglDepthRange can re-emit it
		qvkCmdSetViewport( vk.cmd, 0, 1, &vp );

		sc.offset.x = (int32_t)( x * s );
		sc.offset.y = (int32_t)( yTop * s );	// scissor stays in framebuffer (top-left) coords
		sc.extent.width = (uint32_t)( w * s );
		sc.extent.height = (uint32_t)( h * s );
		qvkCmdSetScissor( vk.cmd, 0, 1, &sc );
	}
}

/*
================
VK_ClearView

RB_BeginDrawingView leaf: clear depth (+ optional stencil/color) within the
current view rectangle, matching GL's per-view qglClear.
================
*/
void VK_ClearView( int clearBits ) {
	VkClearAttachment	att[2];
	VkClearRect			rect;
	int					n = 0;

	if ( !vk.frameStarted || clearBits == 0 ) {
		return;
	}

	memset( att, 0, sizeof( att ) );

	// GL_DEPTH_BUFFER_BIT (0x100) always; GL_STENCIL_BUFFER_BIT (0x400)
	att[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	if ( clearBits & GL_STENCIL_BUFFER_BIT ) {
		att[n].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	att[n].clearValue.depthStencil.depth = 1.0f;
	att[n].clearValue.depthStencil.stencil = 0;
	n++;

	if ( clearBits & GL_COLOR_BUFFER_BIT ) {
		att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		att[n].colorAttachment = 0;
		att[n].clearValue.color.float32[0] = 0.0f;
		att[n].clearValue.color.float32[1] = 0.0f;
		att[n].clearValue.color.float32[2] = 0.0f;
		att[n].clearValue.color.float32[3] = 1.0f;
		n++;
	}

	// clear rect in framebuffer (top-left) coords, scaled by the current target's factor
	{
		float s = vk.curScale;
		int yTop = glConfig.vidHeight - backEnd.viewParms.viewportY - backEnd.viewParms.viewportHeight;
		rect.rect.offset.x = (int32_t)( backEnd.viewParms.viewportX * s );
		rect.rect.offset.y = (int32_t)( yTop * s );
		rect.rect.extent.width = (uint32_t)( backEnd.viewParms.viewportWidth * s );
		rect.rect.extent.height = (uint32_t)( backEnd.viewParms.viewportHeight * s );
	}
	rect.baseArrayLayer = 0;
	rect.layerCount = 1;

	qvkCmdClearAttachments( vk.cmd, n, att, 1, &rect );
}

/*
================
VK_SetModelMatrix

Replaces qglLoadMatrixf(modelMatrix) in the 3D path: build the push-constant MVP
as Cz * projection * model.  Cz only remaps clip Z from GL's [-1,1] to Vulkan's
[0,1]; the GL->Vulkan Y flip is handled by the negative-height viewport (so GL's
CCW winding is preserved and culling stays identical).
================
*/
void VK_SetModelMatrix( const float *modelMatrix ) {
	float	pm[16];
	int		c;

	VK_Mat4Mul( vk.draw.projection, modelMatrix, pm );

	for ( c = 0; c < 4; c++ ) {
		vk.draw.mvp[c * 4 + 0] = pm[c * 4 + 0];
		vk.draw.mvp[c * 4 + 1] = pm[c * 4 + 1];
		vk.draw.mvp[c * 4 + 2] = 0.5f * ( pm[c * 4 + 2] + pm[c * 4 + 3] );	// [-1,1] -> [0,1]
		vk.draw.mvp[c * 4 + 3] = pm[c * 4 + 3];
	}
}

/*
================
VK_State / VK_Cull -- record pipeline state for the next draw
================
*/
void VK_State( unsigned stateBits ) {
	vk.draw.stateBits = stateBits;
	glState.glStateBits = stateBits;
}

void VK_Cull( int cullType ) {
	vk.draw.cullType = cullType;
	glState.faceCulling = cullType;
}

void VK_TexEnv( int env ) {
	vk.draw.multitexEnv = env;
}

// portal/mirror world-space clip plane fed to gl_ClipDistance (NULL disables)
void VK_SetClipPlane( const float *plane ) {
	if ( plane ) {
		vk.draw.clipPlane[0] = plane[0];
		vk.draw.clipPlane[1] = plane[1];
		vk.draw.clipPlane[2] = plane[2];
		vk.draw.clipPlane[3] = plane[3];
	} else {
		vk.draw.clipPlane[0] = vk.draw.clipPlane[1] = vk.draw.clipPlane[2] = vk.draw.clipPlane[3] = 0.0f;
	}
}

// GL texenv mode -> multi.frag combine spec constant (0 MODULATE, 1 ADD, 2 REPLACE)
static byte VK_CombineCode( int glEnv ) {
	switch ( glEnv ) {
	case GL_ADD:		return 1;
	case GL_REPLACE:	return 2;
	default:		return 0;	// GL_MODULATE
	}
}

/*
================
VK_Bind -- record the bound texture for a texture unit
================
*/
void VK_Bind( int tmu, image_t *image ) {
	if ( tmu < 0 || tmu > 1 ) {
		return;
	}
	vk.draw.image[tmu] = image;
}

/*
================
VK_FillLightUbo

Pack this frame's Blinn-Phong light data (the dominant lightgrid light sampled at
the view origin + the active dynamic lights) into the current frame's light UBO.
Runs at most once per frame; the contents are constant across the frame's draws.
================
*/
static void VK_FillLightUbo( void ) {
	vkLightUbo_t	*ubo;
	int				i, n;

	// One ring slot per VIEW: a frame can hold several views (main + mirror/portal
	// sub-views) and backEnd.refdef -- vieworg, dlights -- changes per view.  Reuse
	// the current slot only while the view origin is unchanged; otherwise advance to
	// a fresh slot so already-recorded draws keep their own light data.
	if ( vk.lightUboFilled
			&& vk.lightViewOrigin[0] == backEnd.refdef.vieworg[0]
			&& vk.lightViewOrigin[1] == backEnd.refdef.vieworg[1]
			&& vk.lightViewOrigin[2] == backEnd.refdef.vieworg[2] ) {
		return;
	}
	if ( vk.lightViewSlot < VK_LIGHT_MAX_VIEWS - 1 ) {
		vk.lightViewSlot++;					// clamp at the last slot if a frame ever exceeds the ring
	}
	vk.lightUboOffset = (uint32_t)vk.lightViewSlot * vk.lightUboStride;
	ubo = (vkLightUbo_t *)( (byte *)vk.lightUboMapped[vk.frameIndex] + vk.lightUboOffset );

	VectorCopy( backEnd.refdef.vieworg, ubo->viewOrigin );
	ubo->viewOrigin[3] = 0.0f;

	n = backEnd.refdef.num_dlights;
	if ( n > MAX_DLIGHTS ) {
		n = MAX_DLIGHTS;
	}
	for ( i = 0; i < n; i++ ) {
		const dlight_t *dl = &backEnd.refdef.dlights[i];
		VectorCopy( dl->origin, ubo->dlightPos[i] );
		ubo->dlightPos[i][3] = dl->radius;
		VectorCopy( dl->color, ubo->dlightColor[i] );
		ubo->dlightColor[i][3] = 0.0f;
	}

	ubo->params[0] = tr.identityLight;			// share the lightmap's pre-gamma range
	ubo->params[1] = r_specExponent->value;
	ubo->params[2] = r_specScale->value;
	ubo->params[3] = (float)n;

	VectorCopy( backEnd.refdef.vieworg, vk.lightViewOrigin );
	vk.lightUboFilled = qtrue;
}

/*
================
VK_DrawElements

R_DrawElements leaf: stream the current tess batch into the frame's rings, pick
the pipeline for the recorded state, bind textures + MVP and issue the draw.
================
*/
void VK_DrawElements( int numIndexes, const glIndex_t *indexes ) {
	static vkVertex_t	verts[SHADER_MAX_VERTEXES];
	vkPipelineKey_t		key;
	VkPipeline			pipeline;
	VkPipelineLayout	layout;
	VkDeviceSize		vtxOffset, idxOffset;
	int					numVerts = tess.numVertexes;
	int					numSets;
	int					i;

	if ( !vk.frameStarted || numVerts <= 0 || numIndexes <= 0 ) {
		return;
	}
	if ( numVerts > SHADER_MAX_VERTEXES ) {
		return;
	}

	// build interleaved vertices from whatever client arrays the GL path bound
	// (tess.svars for the generic path, local arrays for dlights, etc.)
	{
		const byte	*xyzBase = (const byte *)vk.draw.xyzPtr;
		const byte	*colBase = (const byte *)vk.draw.colorPtr;
		const byte	*tc0Base = (const byte *)vk.draw.tcPtr[0];
		const byte	*tc1Base = (const byte *)vk.draw.tcPtr[1];
		int			xyzStride = vk.draw.xyzStride ? vk.draw.xyzStride : (int)sizeof( tess.xyz[0] );
		int			colStride = vk.draw.colorStride ? vk.draw.colorStride : 4;
		int			tc0Stride = vk.draw.tcStride[0] ? vk.draw.tcStride[0] : (int)sizeof( vec2_t );
		int			tc1Stride = vk.draw.tcStride[1] ? vk.draw.tcStride[1] : (int)sizeof( vec2_t );

		if ( !xyzBase ) {	// some paths leave xyz implicit -> fall back to tess
			xyzBase = (const byte *)tess.xyz;
			xyzStride = (int)sizeof( tess.xyz[0] );
		}

		for ( i = 0; i < numVerts; i++ ) {
			const float *p = (const float *)( xyzBase + i * xyzStride );
			verts[i].xyz[0] = p[0];
			verts[i].xyz[1] = p[1];
			verts[i].xyz[2] = p[2];

			if ( colBase ) {
				Com_Memcpy( verts[i].color, colBase + i * colStride, 4 );
			} else {
				verts[i].color[0] = verts[i].color[1] = verts[i].color[2] = verts[i].color[3] = 255;
			}

			if ( tc0Base ) {
				const float *t = (const float *)( tc0Base + i * tc0Stride );
				verts[i].tc0[0] = t[0]; verts[i].tc0[1] = t[1];
			} else {
				verts[i].tc0[0] = verts[i].tc0[1] = 0.0f;
			}

			if ( tc1Base ) {
				const float *t = (const float *)( tc1Base + i * tc1Stride );
				verts[i].tc1[0] = t[0]; verts[i].tc1[1] = t[1];
			} else {
				verts[i].tc1[0] = verts[i].tc1[1] = 0.0f;
			}
		}
	}

	if ( !VK_StreamVertexes( verts, numVerts, &vtxOffset ) ) {
		return;
	}
	if ( !VK_StreamIndexes( indexes, numIndexes, &idxOffset ) ) {
		return;
	}

	// TMU0 must always have a texture bound (GL reuses the last binding; Vulkan has
	// no fallback and would draw with an undefined descriptor).  Match GL's default.
	if ( !vk.draw.image[0] || !vk.draw.image[0]->vkData ) {
		vk.draw.image[0] = tr.whiteImage;
	}

	// pipeline key
	Com_Memset( &key, 0, sizeof( key ) );
	key.stateBits = vk.draw.stateBits;
	key.cullType = (byte)vk.draw.cullType;
	key.mirror = backEnd.viewParms.isMirror ? 1 : 0;
	key.shaderType = ( vk.draw.image[1] && vk.draw.multitexEnv ) ? VK_SHADER_MULTI : VK_SHADER_SINGLE;
	key.multitexEnv = VK_CombineCode( vk.draw.multitexEnv );
	key.polygonOffset = ( tess.shader && tess.shader->polygonOffset ) ? 1 : 0;

	// the multitexture shader does not exist yet (Phase 5): fall back to single
	if ( key.shaderType == VK_SHADER_MULTI && !vk.shaderVert[VK_SHADER_MULTI] ) {
		key.shaderType = VK_SHADER_SINGLE;
	}

	// Per-pixel Blinn-Phong (r_perPixelLighting): upgrade lightmapped WORLD surfaces
	// to the lit pipeline, which adds a specular term on top of the diffuse*lightmap
	// base.  World-only (entities bake their lighting into vertex colours, which would
	// double-count); collapsed-multitexture opaque surfaces only; no deforms (those
	// rewrite geometry/normals); needs a lightgrid.  The normal is reconstructed in
	// the shader from world-position derivatives, so no per-vertex normal is required.
	if ( r_perPixelLighting->integer && key.shaderType == VK_SHADER_MULTI
			&& backEnd.refdef.num_dlights > 0	// specular is dlight-only -> skip when no dynamic lights
			&& key.multitexEnv == 0			// MODULATE only (genuine diffuse*lightmap)
			&& !( key.stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )	// unblended opaque base only
			&& vk.draw.image[1] && vk.draw.image[1]->vkData	// lightmap set 1 must be bindable
			&& vk.shaderVert[VK_SHADER_LIT]
			&& backEnd.currentEntity == &tr.worldEntity
			&& !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL )
			&& tess.shader && tess.shader->numDeforms == 0
			&& tr.world && tr.world->lightGridData ) {
		key.shaderType = VK_SHADER_LIT;
	}

	numSets = ( key.shaderType == VK_SHADER_MULTI ) ? 2 : ( key.shaderType == VK_SHADER_LIT ) ? 3 : 1;
	layout = vk.pipelineLayout[numSets];

	pipeline = VK_GetPipeline( &key );
	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );

	qvkCmdPushConstants( vk.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof( float ), vk.draw.mvp );
	qvkCmdPushConstants( vk.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 16 * sizeof( float ), 4 * sizeof( float ), vk.draw.clipPlane );

	// bind textures (TMU0 always; TMU1 for the multitexture pipeline)
	if ( vk.draw.image[0] && vk.draw.image[0]->vkData ) {
		vkimage_t *vki = (vkimage_t *)vk.draw.image[0]->vkData;
		qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &vki->descriptor, 0, NULL );
	}
	if ( numSets >= 2 && vk.draw.image[1] && vk.draw.image[1]->vkData ) {
		vkimage_t *vki = (vkimage_t *)vk.draw.image[1]->vkData;
		qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &vki->descriptor, 0, NULL );
	}
	if ( key.shaderType == VK_SHADER_LIT ) {
		VK_FillLightUbo();		// picks/refills this view's ring slot -> vk.lightUboOffset
		qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1,
			&vk.lightUboSet[vk.frameIndex], 1, &vk.lightUboOffset );
	}

	// depth bias (dynamic state is always present in the pipeline)
	if ( key.polygonOffset ) {
		qvkCmdSetDepthBias( vk.cmd, r_offsetUnits->value, 0.0f, r_offsetFactor->value );
	} else {
		qvkCmdSetDepthBias( vk.cmd, 0.0f, 0.0f, 0.0f );
	}

	qvkCmdBindVertexBuffers( vk.cmd, 0, 1, &vk.vertexBuffer[vk.frameIndex], &vtxOffset );
	qvkCmdBindIndexBuffer( vk.cmd, vk.indexBuffer[vk.frameIndex], idxOffset, VK_INDEX_TYPE_UINT32 );
	qvkCmdDrawIndexed( vk.cmd, numIndexes, 1, 0, 0, 0 );

	// Clear the second texture unit after every draw.  The shared stage iterators
	// only bind TMU1 (via GL_SelectTexture(1)+GL_Bind) for genuine multitexture
	// passes; without this reset a following single-texture draw (weapon, fonts,
	// 2D) would inherit a stale image[1] and wrongly pick the multitexture pipeline,
	// modulating against the previous lightmap -> a black overlay on those surfaces.
	vk.draw.image[1] = NULL;
	vk.draw.multitexEnv = 0;
}

/*
================
VK_SetDefaultState

bk.SetDefaultState leaf -- Vulkan keeps no global fixed-function state.
================
*/
static void VK_SetDefaultState( void ) {
	memset( &glState, 0, sizeof( glState ) );
}

/*
================
VKBE_Install

Point the dispatch table at the Vulkan leaves.  The command-list executor is the
SAME RB_ExecuteRenderCommands as OpenGL -- the RB_* handlers branch to the VK
frame/draw leaves internally.
================
*/
void VKBE_Install( backend_t *b ) {
	b->name                  = "Vulkan";
	b->Init                  = VK_Init;
	b->Shutdown              = VK_Shutdown;
	b->SetDefaultState       = VK_SetDefaultState;
	b->GfxInfo               = VK_GfxInfo;
	b->ExecuteRenderCommands = RB_ExecuteRenderCommands;
	b->CreateImage           = VK_CreateImage;
	b->DeleteImages          = VK_DeleteImages;
	b->TextureMode           = VK_TextureMode;
}
