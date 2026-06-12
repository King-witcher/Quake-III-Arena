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

	if ( !vk.initialized || vk.frameStarted ) {
		return;
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

	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
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
	colorAttachment.imageView = vk.swapchainViews[vk.swapchainIndex];
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
	renderingInfo.renderArea.extent = vk.extent;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;

	qvkCmdBeginRendering( vk.cmd, &renderingInfo );

	memset( &viewport, 0, sizeof( viewport ) );
	viewport.x = 0.0f;
	viewport.y = (float)vk.extent.height;		// negative-height viewport (GL-compatible Y)
	viewport.width = (float)vk.extent.width;
	viewport.height = -(float)vk.extent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	qvkCmdSetViewport( vk.cmd, 0, 1, &viewport );

	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent = vk.extent;
	qvkCmdSetScissor( vk.cmd, 0, 1, &scissor );

	// reset the per-draw recording state
	memset( &vk.draw, 0, sizeof( vk.draw ) );
	glState.glStateBits = 0;
	glState.faceCulling = -1;

	vk.frameStarted = qtrue;
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

	if ( !vk.frameStarted ) {
		return;
	}

	qvkCmdEndRendering( vk.cmd );

	VK_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT );

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
// dispatched GL-leaf equivalents
//
//==========================================================================

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

	if ( vk.frameStarted ) {
		memset( &viewport, 0, sizeof( viewport ) );
		viewport.x = 0.0f;
		viewport.y = h;			// negative-height viewport (flip Y in the viewport transform)
		viewport.width = w;
		viewport.height = -h;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		qvkCmdSetViewport( vk.cmd, 0, 1, &viewport );

		scissor.offset.x = 0;
		scissor.offset.y = 0;
		scissor.extent = vk.extent;
		qvkCmdSetScissor( vk.cmd, 0, 1, &scissor );
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

	memset( &vp, 0, sizeof( vp ) );
	vp.x = (float)x;
	vp.y = (float)( yTop + h );		// negative-height viewport (GL-compatible Y)
	vp.width = (float)w;
	vp.height = -(float)h;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	qvkCmdSetViewport( vk.cmd, 0, 1, &vp );

	sc.offset.x = x;
	sc.offset.y = yTop;				// scissor stays in framebuffer (top-left) coords
	sc.extent.width = w;
	sc.extent.height = h;
	qvkCmdSetScissor( vk.cmd, 0, 1, &sc );
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

	rect.rect.offset.x = backEnd.viewParms.viewportX;
	rect.rect.offset.y = glConfig.vidHeight - backEnd.viewParms.viewportY - backEnd.viewParms.viewportHeight;
	rect.rect.extent.width = backEnd.viewParms.viewportWidth;
	rect.rect.extent.height = backEnd.viewParms.viewportHeight;
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
	numSets = ( key.shaderType == VK_SHADER_MULTI ) ? 2 : 1;
	layout = vk.pipelineLayout[numSets];

	pipeline = VK_GetPipeline( &key );
	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );

	qvkCmdPushConstants( vk.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof( float ), vk.draw.mvp );

	// bind textures (TMU0 always; TMU1 for the multitexture pipeline)
	if ( vk.draw.image[0] && vk.draw.image[0]->vkData ) {
		vkimage_t *vki = (vkimage_t *)vk.draw.image[0]->vkData;
		qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &vki->descriptor, 0, NULL );
	}
	if ( numSets == 2 && vk.draw.image[1] && vk.draw.image[1]->vkData ) {
		vkimage_t *vki = (vkimage_t *)vk.draw.image[1]->vkData;
		qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &vki->descriptor, 0, NULL );
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
