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
// vk_memory.c -- device-memory helpers and the per-frame host-visible vertex /
// index streaming rings that the dynamic tess geometry is uploaded through.
//
#include "vk_local.h"

/*
================
VK_AllocBufferMemory

Allocate and bind memory for a buffer; optionally persistently map it.
================
*/
VkDeviceMemory VK_AllocBufferMemory( VkBuffer buffer, VkMemoryPropertyFlags props, void **mapped ) {
	VkMemoryRequirements	memReq;
	VkMemoryAllocateInfo	allocInfo;
	VkDeviceMemory			memory;

	qvkGetBufferMemoryRequirements( vk.device, buffer, &memReq );

	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType( memReq.memoryTypeBits, props );

	VK_CHECK( qvkAllocateMemory( vk.device, &allocInfo, NULL, &memory ) );
	VK_CHECK( qvkBindBufferMemory( vk.device, buffer, memory, 0 ) );

	if ( mapped ) {
		VK_CHECK( qvkMapMemory( vk.device, memory, 0, VK_WHOLE_SIZE, 0, mapped ) );
	}

	return memory;
}

static VkBuffer VK_CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage ) {
	VkBufferCreateInfo	info;
	VkBuffer			buffer;

	memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK( qvkCreateBuffer( vk.device, &info, NULL, &buffer ) );
	return buffer;
}

/*
================
VK_CreateStreamingBuffers

One vertex and one index ring per frame-in-flight, host-visible + coherent and
persistently mapped so the CPU can append geometry without any staging copy.
================
*/
qboolean VK_CreateStreamingBuffers( void ) {
	VkMemoryPropertyFlags	hostProps =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	int i;

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		vk.vertexBuffer[i] = VK_CreateBuffer( VK_VERTEX_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT );
		vk.vertexMemory[i] = VK_AllocBufferMemory( vk.vertexBuffer[i], hostProps, (void **)&vk.vertexMapped[i] );

		vk.indexBuffer[i] = VK_CreateBuffer( VK_INDEX_BUFFER_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT );
		vk.indexMemory[i] = VK_AllocBufferMemory( vk.indexBuffer[i], hostProps, (void **)&vk.indexMapped[i] );
	}

	vk.vertexOffset = 0;
	vk.indexOffset = 0;
	return qtrue;
}

/*
================
VK_DestroyStreamingBuffers
================
*/
void VK_DestroyStreamingBuffers( void ) {
	int i;

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		if ( vk.vertexMemory[i] ) {
			qvkUnmapMemory( vk.device, vk.vertexMemory[i] );
			qvkFreeMemory( vk.device, vk.vertexMemory[i], NULL );
			vk.vertexMemory[i] = VK_NULL_HANDLE;
			vk.vertexMapped[i] = NULL;
		}
		if ( vk.vertexBuffer[i] ) {
			qvkDestroyBuffer( vk.device, vk.vertexBuffer[i], NULL );
			vk.vertexBuffer[i] = VK_NULL_HANDLE;
		}
		if ( vk.indexMemory[i] ) {
			qvkUnmapMemory( vk.device, vk.indexMemory[i] );
			qvkFreeMemory( vk.device, vk.indexMemory[i], NULL );
			vk.indexMemory[i] = VK_NULL_HANDLE;
			vk.indexMapped[i] = NULL;
		}
		if ( vk.indexBuffer[i] ) {
			qvkDestroyBuffer( vk.device, vk.indexBuffer[i], NULL );
			vk.indexBuffer[i] = VK_NULL_HANDLE;
		}
	}
}

/*
================
VK_ResetStreaming

Rewind the current frame's rings.  Safe because the in-flight fence guarantees
the GPU has finished reading this frame slot before we reuse it.
================
*/
void VK_ResetStreaming( void ) {
	vk.vertexOffset = 0;
	vk.indexOffset = 0;
}

/*
================
VK_StreamVertexes

Append interleaved vertices to the current frame's vertex ring.  *outOffset is
the byte offset to bind at; returns qfalse (and drops the draw) on overflow.
================
*/
qboolean VK_StreamVertexes( const vkVertex_t *verts, int count, VkDeviceSize *outOffset ) {
	uint32_t bytes = (uint32_t)( count * sizeof( vkVertex_t ) );

	if ( vk.vertexOffset + bytes > VK_VERTEX_BUFFER_SIZE ) {
		ri.Printf( PRINT_DEVELOPER, "VK_StreamVertexes: vertex ring overflow\n" );
		return qfalse;
	}

	Com_Memcpy( vk.vertexMapped[vk.frameIndex] + vk.vertexOffset, verts, bytes );
	*outOffset = vk.vertexOffset;
	vk.vertexOffset += bytes;
	return qtrue;
}

/*
================
VK_StreamIndexes
================
*/
qboolean VK_StreamIndexes( const glIndex_t *indexes, int count, VkDeviceSize *outOffset ) {
	uint32_t bytes = (uint32_t)( count * sizeof( glIndex_t ) );

	if ( vk.indexOffset + bytes > VK_INDEX_BUFFER_SIZE ) {
		ri.Printf( PRINT_DEVELOPER, "VK_StreamIndexes: index ring overflow\n" );
		return qfalse;
	}

	Com_Memcpy( vk.indexMapped[vk.frameIndex] + vk.indexOffset, indexes, bytes );
	*outOffset = vk.indexOffset;
	vk.indexOffset += bytes;
	return qtrue;
}
