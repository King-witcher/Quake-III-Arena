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
// vk_image.c -- Vulkan texture upload, sampler cache and per-image descriptor
// sets.  The CPU pixel processing (power-of-two resample, picmip, gamma/intensity
// scaling, box mip generation) is reused verbatim from tr_image.c, so the bytes
// uploaded are identical to the OpenGL backend -- only the GPU upload differs.
//
#include "vk_local.h"

// gl_filter_min / gl_filter_max live in tr_image.c and hold the active filter
// (shared with GL_TextureMode); we translate them to Vulkan sampler state.
extern int	gl_filter_min;
extern int	gl_filter_max;

#define VK_MAX_SAMPLERS		32

typedef struct {
	int			wrapClampMode;
	int			filterMin;
	int			filterMax;
	qboolean	mipmap;
	qboolean	border;			// CLAMP_TO_BORDER + opaque white (the *fog image)
	VkSampler	sampler;
} vkSamplerCacheEntry_t;

static vkSamplerCacheEntry_t	s_samplers[VK_MAX_SAMPLERS];
static int						s_numSamplers;

//
// Batched texture upload.  Texture creation during a level load issues hundreds
// of small uploads; doing one submit+wait per texture (the naive path) stalls the
// GPU hundreds of times and makes loads crawl.  Instead we record every copy into
// a single command buffer and flush it (one submit+wait) lazily: when the pending
// staging memory grows past a cap, or at the start of the next frame.
//
#define VK_MAX_PENDING_STAGING		1024
#define VK_UPLOAD_FLUSH_BYTES		( 48 * 1024 * 1024 )

static VkCommandBuffer	s_uploadCmd;
static qboolean			s_uploading;
static VkBuffer			s_stagingBufs[VK_MAX_PENDING_STAGING];
static VkDeviceMemory	s_stagingMems[VK_MAX_PENDING_STAGING];
static int				s_numStaging;
static VkDeviceSize		s_stagingBytes;

/*
================
VK_GetSampler

Return a cached VkSampler matching the requested wrap/filter/mip, creating it on
first use.  Translates the Q3 GL filter enums to Vulkan filter + mipmap modes.
================
*/
static VkSampler VK_GetSampler( int wrapClampMode, qboolean mipmap, qboolean border ) {
	VkSamplerCreateInfo	info;
	VkSampler			sampler;
	int					i;
	int					filterMin = mipmap ? gl_filter_min : GL_LINEAR;
	int					filterMax = mipmap ? gl_filter_max : GL_LINEAR;

	for ( i = 0; i < s_numSamplers; i++ ) {
		if ( s_samplers[i].wrapClampMode == wrapClampMode &&
			 s_samplers[i].filterMin == filterMin &&
			 s_samplers[i].filterMax == filterMax &&
			 s_samplers[i].mipmap == mipmap &&
			 s_samplers[i].border == border ) {
			return s_samplers[i].sampler;
		}
	}

	memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

	// magnification filter
	info.magFilter = ( filterMax == GL_LINEAR ) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

	// minification base filter + mipmap mode
	switch ( filterMin ) {
	case GL_NEAREST:
		info.minFilter = VK_FILTER_NEAREST; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
	case GL_LINEAR:
		info.minFilter = VK_FILTER_LINEAR;  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
	case GL_NEAREST_MIPMAP_NEAREST:
		info.minFilter = VK_FILTER_NEAREST; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
	case GL_LINEAR_MIPMAP_NEAREST:
		info.minFilter = VK_FILTER_LINEAR;  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
	case GL_NEAREST_MIPMAP_LINEAR:
		info.minFilter = VK_FILTER_NEAREST; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  break;
	default:
	case GL_LINEAR_MIPMAP_LINEAR:
		info.minFilter = VK_FILTER_LINEAR;  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  break;
	}

	if ( border ) {
		// the fog density LUT samples opaque white outside [0,1] (full fog)
		info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	} else if ( wrapClampMode == GL_CLAMP ) {
		info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	} else {
		info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
	info.mipLodBias = 0.0f;
	info.anisotropyEnable = VK_FALSE;
	info.maxAnisotropy = 1.0f;
	info.minLod = 0.0f;
	info.maxLod = mipmap ? VK_LOD_CLAMP_NONE : 0.0f;
	info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

	VK_CHECK( qvkCreateSampler( vk.device, &info, NULL, &sampler ) );

	if ( s_numSamplers < VK_MAX_SAMPLERS ) {
		s_samplers[s_numSamplers].wrapClampMode = wrapClampMode;
		s_samplers[s_numSamplers].filterMin = filterMin;
		s_samplers[s_numSamplers].filterMax = filterMax;
		s_samplers[s_numSamplers].mipmap = mipmap;
		s_samplers[s_numSamplers].border = border;
		s_samplers[s_numSamplers].sampler = sampler;
		s_numSamplers++;
	}
	return sampler;
}

static void VK_DestroySamplers( void ) {
	int i;
	for ( i = 0; i < s_numSamplers; i++ ) {
		qvkDestroySampler( vk.device, s_samplers[i].sampler, NULL );
	}
	s_numSamplers = 0;
	memset( s_samplers, 0, sizeof( s_samplers ) );
}

/*
================
VK_InitImageSystem

Descriptor pool large enough for one combined-image-sampler set per image.
================
*/
qboolean VK_InitImageSystem( void ) {
	VkDescriptorPoolSize		poolSize;
	VkDescriptorPoolCreateInfo	poolInfo;

	memset( &poolSize, 0, sizeof( poolSize ) );
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = MAX_DRAWIMAGES;

	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolInfo.maxSets = MAX_DRAWIMAGES;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;

	VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vk.descriptorPool ) );

	s_numSamplers = 0;
	return qtrue;
}

void VK_ShutdownImageSystem( void ) {
	VK_FlushUploads();
	VK_DestroySamplers();
	if ( vk.descriptorPool ) {
		qvkDestroyDescriptorPool( vk.device, vk.descriptorPool, NULL );
		vk.descriptorPool = VK_NULL_HANDLE;
	}
}

/*
================
VK_UpdateImageDescriptor

(Re)point an image's descriptor set at its view + the right cached sampler.
================
*/
static void VK_UpdateImageDescriptor( image_t *image ) {
	vkimage_t				*vki = (vkimage_t *)image->vkData;
	VkDescriptorImageInfo	imageInfo;
	VkWriteDescriptorSet	write;

	// the fog density LUT (*fog) needs CLAMP_TO_BORDER with opaque-white border
	vki->sampler = VK_GetSampler( image->wrapClampMode, image->mipmap,
		(qboolean)( Q_stricmp( image->imgName, "*fog" ) == 0 ) );

	memset( &imageInfo, 0, sizeof( imageInfo ) );
	imageInfo.sampler = vki->sampler;
	imageInfo.imageView = vki->view;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset( &write, 0, sizeof( write ) );
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = vki->descriptor;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;

	qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
}

/*
================
VK_BeginUploadBatch

Lazily open the shared upload command buffer.
================
*/
static void VK_BeginUploadBatch( void ) {
	VkCommandBufferAllocateInfo	cbAlloc;
	VkCommandBufferBeginInfo	begin;

	if ( s_uploading ) {
		return;
	}

	memset( &cbAlloc, 0, sizeof( cbAlloc ) );
	cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbAlloc.commandPool = vk.commandPool;
	cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbAlloc.commandBufferCount = 1;
	VK_CHECK( qvkAllocateCommandBuffers( vk.device, &cbAlloc, &s_uploadCmd ) );

	memset( &begin, 0, sizeof( begin ) );
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	qvkBeginCommandBuffer( s_uploadCmd, &begin );

	s_uploading = qtrue;
	s_numStaging = 0;
	s_stagingBytes = 0;
}

/*
================
VK_FlushUploads

Submit the pending upload command buffer (one wait for the whole batch) and free
its staging buffers.  Called at frame start and when the batch grows too large.
================
*/
void VK_FlushUploads( void ) {
	VkSubmitInfo	submit;
	int				i;

	if ( !s_uploading ) {
		return;
	}

	qvkEndCommandBuffer( s_uploadCmd );

	memset( &submit, 0, sizeof( submit ) );
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &s_uploadCmd;
	VK_CHECK( qvkQueueSubmit( vk.graphicsQueue, 1, &submit, VK_NULL_HANDLE ) );
	qvkQueueWaitIdle( vk.graphicsQueue );

	qvkFreeCommandBuffers( vk.device, vk.commandPool, 1, &s_uploadCmd );
	s_uploadCmd = VK_NULL_HANDLE;

	for ( i = 0; i < s_numStaging; i++ ) {
		qvkUnmapMemory( vk.device, s_stagingMems[i] );	// now that the GPU is done
		qvkFreeMemory( vk.device, s_stagingMems[i], NULL );
		qvkDestroyBuffer( vk.device, s_stagingBufs[i], NULL );
	}
	s_numStaging = 0;
	s_stagingBytes = 0;
	s_uploading = qfalse;
}

/*
================
VK_RecordUpload

Stage a full mip chain and record its copy + layout transitions into the shared
upload command buffer.  The staging buffer is freed at the next VK_FlushUploads.
================
*/
static void VK_RecordUpload( VkImage image, int levels, int *mipWidth, int *mipHeight,
	byte **mipData, VkDeviceSize totalBytes ) {
	VkBuffer				staging;
	VkDeviceMemory			stagingMem;
	VkBufferCreateInfo		bufInfo;
	byte					*mapped;
	VkBufferImageCopy		regions[16];
	VkDeviceSize			offset;
	int						i;
	VkImageMemoryBarrier	barrier;

	VK_BeginUploadBatch();

	memset( &bufInfo, 0, sizeof( bufInfo ) );
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size = totalBytes;
	bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK( qvkCreateBuffer( vk.device, &bufInfo, NULL, &staging ) );
	stagingMem = VK_AllocBufferMemory( staging,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, (void **)&mapped );

	offset = 0;
	for ( i = 0; i < levels; i++ ) {
		VkDeviceSize size = (VkDeviceSize)mipWidth[i] * mipHeight[i] * 4;
		Com_Memcpy( mapped + offset, mipData[i], (size_t)size );

		memset( &regions[i], 0, sizeof( regions[i] ) );
		regions[i].bufferOffset = offset;
		regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		regions[i].imageSubresource.mipLevel = i;
		regions[i].imageSubresource.layerCount = 1;
		regions[i].imageExtent.width = mipWidth[i];
		regions[i].imageExtent.height = mipHeight[i];
		regions[i].imageExtent.depth = 1;

		offset += size;
	}
	// keep the staging memory mapped until the batch is flushed (unmapping a
	// host-coherent allocation before the GPU has consumed it can leave writes in
	// CPU write-combine buffers on some drivers -> corrupted/striped textures)

	memset( &barrier, 0, sizeof( barrier ) );
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = levels;
	barrier.subresourceRange.layerCount = 1;

	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	qvkCmdPipelineBarrier( s_uploadCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier );

	qvkCmdCopyBufferToImage( s_uploadCmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions );

	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	qvkCmdPipelineBarrier( s_uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier );

	// keep the staging buffer alive until the batch is flushed (we always flush
	// before the table fills, so s_numStaging is always in range here)
	s_stagingBufs[s_numStaging] = staging;
	s_stagingMems[s_numStaging] = stagingMem;
	s_numStaging++;
	s_stagingBytes += totalBytes;

	if ( s_stagingBytes >= VK_UPLOAD_FLUSH_BYTES || s_numStaging >= VK_MAX_PENDING_STAGING ) {
		VK_FlushUploads();
	}
}

/*
================
VK_CreateImage

bk.CreateImage leaf.  Mirrors Upload32's CPU processing exactly, then creates the
VkImage / view / descriptor and uploads the mip chain.
================
*/
void VK_CreateImage( image_t *image, const byte *pic, qboolean isLightmap ) {
	vkimage_t				*vki;
	unsigned				*data = (unsigned *)pic;
	unsigned				*resampledBuffer = NULL;
	unsigned				*scaledBuffer;
	int						width = image->width;
	int						height = image->height;
	int						scaled_width, scaled_height;
	int						mipWidth[16], mipHeight[16];
	byte					*mipData[16];
	int						levels;
	VkDeviceSize			totalBytes;
	int						i;
	VkImageCreateInfo		imageInfo;
	VkMemoryRequirements	memReq;
	VkMemoryAllocateInfo	allocInfo;
	VkImageViewCreateInfo	viewInfo;
	VkDescriptorSetAllocateInfo	dsAlloc;

	// --- the following block reproduces Upload32 (tr_image.c) for pixel parity ---

	for ( scaled_width = 1; scaled_width < width; scaled_width <<= 1 )
		;
	for ( scaled_height = 1; scaled_height < height; scaled_height <<= 1 )
		;
	if ( r_roundImagesDown->integer && scaled_width > width )
		scaled_width >>= 1;
	if ( r_roundImagesDown->integer && scaled_height > height )
		scaled_height >>= 1;

	if ( scaled_width != width || scaled_height != height ) {
		resampledBuffer = ri.Hunk_AllocateTempMemory( scaled_width * scaled_height * 4 );
		ResampleTexture( data, width, height, resampledBuffer, scaled_width, scaled_height );
		data = resampledBuffer;
		width = scaled_width;
		height = scaled_height;
	}

	if ( image->allowPicmip ) {
		scaled_width >>= r_picmip->integer;
		scaled_height >>= r_picmip->integer;
	}
	if ( scaled_width < 1 ) scaled_width = 1;
	if ( scaled_height < 1 ) scaled_height = 1;
	while ( scaled_width > glConfig.maxTextureSize || scaled_height > glConfig.maxTextureSize ) {
		scaled_width >>= 1;
		scaled_height >>= 1;
	}

	scaledBuffer = ri.Hunk_AllocateTempMemory( sizeof( unsigned ) * scaled_width * scaled_height );

	if ( scaled_width == width && scaled_height == height ) {
		Com_Memcpy( scaledBuffer, data, width * height * 4 );
	} else {
		while ( width > scaled_width || height > scaled_height ) {
			R_MipMap( (byte *)data, width, height );
			width >>= 1; height >>= 1;
			if ( width < 1 ) width = 1;
			if ( height < 1 ) height = 1;
		}
		Com_Memcpy( scaledBuffer, data, width * height * 4 );
	}

	R_LightScaleTexture( scaledBuffer, scaled_width, scaled_height, !image->mipmap );

	image->uploadWidth = scaled_width;
	image->uploadHeight = scaled_height;
	image->internalFormat = 4;		// RGBA

	// build the CPU mip chain (each level is a freshly-allocated copy, because
	// R_MipMap halves scaledBuffer in place)
	levels = 0;
	{
		int w = scaled_width, h = scaled_height;
		totalBytes = 0;
		while ( 1 ) {
			int sz = w * h * 4;
			mipWidth[levels] = w;
			mipHeight[levels] = h;
			mipData[levels] = ri.Hunk_AllocateTempMemory( sz );
			Com_Memcpy( mipData[levels], scaledBuffer, sz );
			totalBytes += sz;
			levels++;
			if ( !image->mipmap || ( w == 1 && h == 1 ) || levels >= 16 ) {
				break;
			}
			R_MipMap( (byte *)scaledBuffer, w, h );
			w >>= 1; h >>= 1;
			if ( w < 1 ) w = 1;
			if ( h < 1 ) h = 1;
		}
	}

	// --- create the Vulkan image, memory, view ---
	vki = ri.Hunk_Alloc( sizeof( vkimage_t ), h_low );
	image->vkData = vki;

	memset( &imageInfo, 0, sizeof( imageInfo ) );
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent.width = scaled_width;
	imageInfo.extent.height = scaled_height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = levels;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK( qvkCreateImage( vk.device, &imageInfo, NULL, &vki->image ) );

	qvkGetImageMemoryRequirements( vk.device, vki->image, &memReq );
	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType( memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CHECK( qvkAllocateMemory( vk.device, &allocInfo, NULL, &vki->memory ) );
	VK_CHECK( qvkBindImageMemory( vk.device, vki->image, vki->memory, 0 ) );

	VK_RecordUpload( vki->image, levels, mipWidth, mipHeight, mipData, totalBytes );

	memset( &viewInfo, 0, sizeof( viewInfo ) );
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = vki->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = levels;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &vki->view ) );

	// per-image descriptor set
	memset( &dsAlloc, 0, sizeof( dsAlloc ) );
	dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsAlloc.descriptorPool = vk.descriptorPool;
	dsAlloc.descriptorSetCount = 1;
	dsAlloc.pSetLayouts = &vk.descriptorSetLayout;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &dsAlloc, &vki->descriptor ) );

	VK_UpdateImageDescriptor( image );

	// If a texture is created while a frame is being recorded (e.g. a model that
	// first appears mid-game), its deferred upload must complete before that frame
	// can sample it -- flush now.  During a level load no frame is active, so this
	// is skipped and uploads stay batched (fast).
	if ( vk.frameStarted ) {
		VK_FlushUploads();
	}

	// free temporaries (reverse order of allocation)
	for ( i = levels - 1; i >= 0; i-- ) {
		ri.Hunk_FreeTempMemory( mipData[i] );
	}
	ri.Hunk_FreeTempMemory( scaledBuffer );
	if ( resampledBuffer ) {
		ri.Hunk_FreeTempMemory( resampledBuffer );
	}
}

/*
================
VK_DeleteImages

bk.DeleteImages leaf: destroy every image's GPU resources.  tr.images[] is still
valid here (R_DeleteTextures clears it afterwards).  Descriptor sets are freed by
resetting the pool.
================
*/
void VK_DeleteImages( void ) {
	int i;

	if ( !vk.device ) {
		return;
	}
	VK_FlushUploads();			// finish any in-flight uploads before freeing images
	qvkDeviceWaitIdle( vk.device );

	for ( i = 0; i < tr.numImages; i++ ) {
		vkimage_t *vki = (vkimage_t *)tr.images[i]->vkData;
		if ( !vki ) {
			continue;
		}
		if ( vki->view )   qvkDestroyImageView( vk.device, vki->view, NULL );
		if ( vki->image )  qvkDestroyImage( vk.device, vki->image, NULL );
		if ( vki->memory ) qvkFreeMemory( vk.device, vki->memory, NULL );
		tr.images[i]->vkData = NULL;
	}

	// reset the descriptor pool to release all per-image sets at once
	if ( vk.descriptorPool ) {
		qvkDestroyDescriptorPool( vk.device, vk.descriptorPool, NULL );
		vk.descriptorPool = VK_NULL_HANDLE;
		VK_InitImageSystem();	// fresh empty pool for the next level
	}
}

/*
================
VK_TextureMode

bk.TextureMode leaf: change the active filter, rebuild the sampler cache and
re-point every mipmapped image's descriptor at the new sampler.
================
*/
void VK_TextureMode( const char *string ) {
	static const struct { const char *name; int min, max; } modes[] = {
		{ "GL_NEAREST",                GL_NEAREST,                GL_NEAREST },
		{ "GL_LINEAR",                 GL_LINEAR,                 GL_LINEAR },
		{ "GL_NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST },
		{ "GL_LINEAR_MIPMAP_NEAREST",  GL_LINEAR_MIPMAP_NEAREST,  GL_LINEAR },
		{ "GL_NEAREST_MIPMAP_LINEAR",  GL_NEAREST_MIPMAP_LINEAR,  GL_NEAREST },
		{ "GL_LINEAR_MIPMAP_LINEAR",   GL_LINEAR_MIPMAP_LINEAR,   GL_LINEAR },
	};
	int i;

	for ( i = 0; i < 6; i++ ) {
		if ( !Q_stricmp( modes[i].name, string ) ) {
			break;
		}
	}
	if ( i == 6 ) {
		ri.Printf( PRINT_ALL, "bad filter name\n" );
		return;
	}

	gl_filter_min = modes[i].min;
	gl_filter_max = modes[i].max;

	if ( !vk.device ) {
		return;
	}
	qvkDeviceWaitIdle( vk.device );

	// new samplers, then re-point mipmapped images at them
	VK_DestroySamplers();
	for ( i = 0; i < tr.numImages; i++ ) {
		if ( tr.images[i]->mipmap && tr.images[i]->vkData ) {
			VK_UpdateImageDescriptor( tr.images[i] );
		}
	}
}
