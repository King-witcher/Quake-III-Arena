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
// vk_backend.c -- the Vulkan command-list executor and frame loop.
//
// This is the Vulkan counterpart of the GL-emitting half of tr_backend.c.  The
// front-end builds the exact same RC_* command list regardless of backend; here
// we drain it by recording a Vulkan command buffer and presenting.
//
#include "vk_local.h"

#define VK_TIMEOUT_NS	( (uint64_t)1000000000 * 5 )	// 5s acquire/fence timeout

//
// transient color set by RC_SET_COLOR, consumed by 2D drawing (Phase 2+)
//
static float	vk_color2D[4] = { 1, 1, 1, 1 };

/*
================
VK_ImageBarrier

Single-image layout transition recorded into the active command buffer.
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
dynamic-rendering pass that clears color + depth.  Invoked from RC_DRAW_BUFFER.
================
*/
static void VK_BeginFrame( void ) {
	VkCommandBufferBeginInfo	beginInfo;
	VkRenderingAttachmentInfo	colorAttachment;
	VkRenderingAttachmentInfo	depthAttachment;
	VkRenderingInfo				renderingInfo;
	VkViewport					viewport;
	VkRect2D					scissor;
	VkResult					res;
	int							frame = vk.frameIndex;

	if ( !vk.initialized ) {
		return;
	}

	// make sure we have a valid swapchain (window may have been resized/minimized)
	if ( !vk.swapchainValid ) {
		if ( !VK_RecreateSwapchain() ) {
			return;	// still not presentable (e.g. minimized)
		}
	}

	// wait until the GPU is done with this frame slot
	qvkWaitForFences( vk.device, 1, &vk.frameFence[frame], VK_TRUE, VK_TIMEOUT_NS );

	res = qvkAcquireNextImageKHR( vk.device, vk.swapchain, VK_TIMEOUT_NS,
		vk.imageAcquired[frame], VK_NULL_HANDLE, &vk.swapchainIndex );
	if ( res == VK_ERROR_OUT_OF_DATE_KHR ) {
		VK_RecreateSwapchain();
		return;	// skip this frame; the acquire semaphore was not signaled
	}
	if ( res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR ) {
		ri.Printf( PRINT_ALL, "vkAcquireNextImageKHR: %s\n", VK_ResultString( res ) );
		return;
	}

	vk.cmd = vk.commandBuffers[frame];
	qvkResetCommandBuffer( vk.cmd, 0 );

	memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	qvkBeginCommandBuffer( vk.cmd, &beginInfo );

	// swapchain image: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );

	// depth image: UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL (contents cleared below)
	VK_ImageBarrier( vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT );

	memset( &colorAttachment, 0, sizeof( colorAttachment ) );
	colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colorAttachment.imageView = vk.swapchainViews[vk.swapchainIndex];
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	// Phase 1 proof-of-life clear colour (a distinctive blue).  Later phases
	// switch this to Q3's actual clear behaviour.
	colorAttachment.clearValue.color.float32[0] = 0.10f;
	colorAttachment.clearValue.color.float32[1] = 0.20f;
	colorAttachment.clearValue.color.float32[2] = 0.35f;
	colorAttachment.clearValue.color.float32[3] = 1.0f;

	memset( &depthAttachment, 0, sizeof( depthAttachment ) );
	depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	depthAttachment.imageView = vk.depthView;
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.clearValue.depthStencil.depth = 1.0f;
	depthAttachment.clearValue.depthStencil.stencil = 0;

	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.offset.x = 0;
	renderingInfo.renderArea.offset.y = 0;
	renderingInfo.renderArea.extent = vk.extent;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;

	qvkCmdBeginRendering( vk.cmd, &renderingInfo );

	// default full-screen viewport / scissor (dynamic state)
	memset( &viewport, 0, sizeof( viewport ) );
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)vk.extent.width;
	viewport.height = (float)vk.extent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	qvkCmdSetViewport( vk.cmd, 0, 1, &viewport );

	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent = vk.extent;
	qvkCmdSetScissor( vk.cmd, 0, 1, &scissor );

	vk.frameStarted = qtrue;
}

/*
================
VK_EndFrame

Close the dynamic-rendering pass, submit and present.  Invoked from
RC_SWAP_BUFFERS.
================
*/
static void VK_EndFrame( void ) {
	VkSubmitInfo			submitInfo;
	VkPresentInfoKHR		presentInfo;
	VkPipelineStageFlags	waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkResult				res;
	int						frame = vk.frameIndex;

	if ( !vk.frameStarted ) {
		return;	// frame was skipped (swapchain out of date / minimized)
	}

	qvkCmdEndRendering( vk.cmd );

	// swapchain image: COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC
	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT );

	qvkEndCommandBuffer( vk.cmd );

	// reset the fence only now that we are certain to submit
	qvkResetFences( vk.device, 1, &vk.frameFence[frame] );

	memset( &submitInfo, 0, sizeof( submitInfo ) );
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &vk.imageAcquired[frame];
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &vk.cmd;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &vk.renderComplete[frame];
	VK_CHECK( qvkQueueSubmit( vk.graphicsQueue, 1, &submitInfo, vk.frameFence[frame] ) );

	memset( &presentInfo, 0, sizeof( presentInfo ) );
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &vk.renderComplete[frame];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &vk.swapchain;
	presentInfo.pImageIndices = &vk.swapchainIndex;

	res = qvkQueuePresentKHR( vk.presentQueue, &presentInfo );
	if ( res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR ) {
		VK_RecreateSwapchain();
	} else if ( res != VK_SUCCESS ) {
		ri.Printf( PRINT_ALL, "vkQueuePresentKHR: %s\n", VK_ResultString( res ) );
	}

	vk.frameStarted = qfalse;
	vk.frameIndex = ( frame + 1 ) % VK_NUM_FRAMES;
}

//==========================================================================
//
// command list executor
//
//==========================================================================

static const void *VK_SetColor( const void *data ) {
	const setColorCommand_t *cmd = (const setColorCommand_t *)data;
	vk_color2D[0] = cmd->color[0];
	vk_color2D[1] = cmd->color[1];
	vk_color2D[2] = cmd->color[2];
	vk_color2D[3] = cmd->color[3];
	return (const void *)(cmd + 1);
}

static const void *VK_StretchPic( const void *data ) {
	const stretchPicCommand_t *cmd = (const stretchPicCommand_t *)data;
	// Phase 2 will record real 2D geometry here.
	return (const void *)(cmd + 1);
}

static const void *VK_DrawSurfs( const void *data ) {
	const drawSurfsCommand_t *cmd = (const drawSurfsCommand_t *)data;
	// Phase 4 will record the 3D world/entity draws here.
	return (const void *)(cmd + 1);
}

static const void *VK_DrawBuffer( const void *data ) {
	const drawBufferCommand_t *cmd = (const drawBufferCommand_t *)data;
	VK_BeginFrame();
	return (const void *)(cmd + 1);
}

static const void *VK_SwapBuffers( const void *data ) {
	const swapBuffersCommand_t *cmd = (const swapBuffersCommand_t *)data;
	VK_EndFrame();
	return (const void *)(cmd + 1);
}

/*
================
VK_ExecuteRenderCommands

bk.ExecuteRenderCommands leaf -- mirrors RB_ExecuteRenderCommands.
================
*/
static void VK_ExecuteRenderCommands( const void *data ) {
	int t1, t2;

	t1 = ri.Milliseconds();
	backEnd.smpFrame = 0;	// SMP is disabled for the Vulkan backend

	while ( 1 ) {
		switch ( *(const int *)data ) {
		case RC_SET_COLOR:
			data = VK_SetColor( data );
			break;
		case RC_STRETCH_PIC:
			data = VK_StretchPic( data );
			break;
		case RC_DRAW_SURFS:
			data = VK_DrawSurfs( data );
			break;
		case RC_DRAW_BUFFER:
			data = VK_DrawBuffer( data );
			break;
		case RC_SWAP_BUFFERS:
			data = VK_SwapBuffers( data );
			break;
		case RC_SCREENSHOT:
			// Phase 8: vkCmd readback.  Skip the command for now.
			data = (const void *)( (const screenshotCommand_t *)data + 1 );
			break;
		case RC_END_OF_LIST:
		default:
			t2 = ri.Milliseconds();
			backEnd.pc.msec = t2 - t1;
			return;
		}
	}
}

//==========================================================================
//
// texture / state leaves (real implementations arrive in Phase 3+)
//
//==========================================================================

/*
================
VK_CreateImage

Phase 1 stub: record the upload dimensions so the shared image_t bookkeeping and
glConfig-driven sizing stay correct, but do not touch the GPU yet.  Phase 3
replaces this with a real VkImage + staging upload.
================
*/
static void VK_CreateImage( image_t *image, const byte *pic, qboolean isLightmap ) {
	image->uploadWidth = image->width;
	image->uploadHeight = image->height;
	image->internalFormat = 4;		// RGBA
	image->vkData = NULL;
}

static void VK_DeleteImages( void ) {
	// Phase 3: destroy VkImages / free descriptor sets.  No GPU objects yet.
}

static void VK_TextureMode( const char *string ) {
	// Phase 3: rebuild the sampler cache.  No-op until samplers exist.
}

static void VK_SetDefaultState( void ) {
	// Vulkan keeps no global fixed-function state; pipeline objects carry it.
	memset( &glState, 0, sizeof( glState ) );
}

/*
================
VKBE_Install

Point the dispatch table at the Vulkan leaves.
================
*/
void VKBE_Install( backend_t *b ) {
	b->name                  = "Vulkan";
	b->Init                  = VK_Init;
	b->Shutdown              = VK_Shutdown;
	b->SetDefaultState       = VK_SetDefaultState;
	b->GfxInfo               = VK_GfxInfo;
	b->ExecuteRenderCommands = VK_ExecuteRenderCommands;
	b->CreateImage           = VK_CreateImage;
	b->DeleteImages          = VK_DeleteImages;
	b->TextureMode           = VK_TextureMode;
}
