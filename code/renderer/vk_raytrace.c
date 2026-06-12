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
// vk_raytrace.c -- hardware ray-traced lighting (VK_KHR_ray_query).  See
// vk_raytrace.h for the high-level design.  This file owns all ray-tracing
// specific Vulkan objects so the rest of the backend stays unaware of them.
//
// Phase 0 (this commit): device-capability detection, extension/feature
// requests, and resolution of the optional KHR entry points.  No acceleration
// structure is built and no rays are traced yet -- with r_raytracing 0 (the
// default) nothing here runs and the image is byte-for-byte the old raster path.
//
#include "vk_local.h"
#include "vk_raytrace.h"
#include "vk_spv.h"			// vk_spv_rt_light_comp[] (the compute lighting shader)

#define RT_MAX_DLIGHTS	32	// must match MAX_RT_DLIGHTS in shaders/rt_light.comp

// Per-frame uniform block; layout MUST match the std140 UBO in rt_light.comp.
typedef struct {
	float	posRad[4];		// xyz origin, w radius
	float	color[4];		// rgb colour
} rtDLight_t;

typedef struct {
	float		invViewProj[16];
	float		eye[4];			// xyz world eye
	float		screen[4];		// w, h, 1/w, 1/h
	float		p0[4];			// ambientScale, giIntensity, numGiRays, giEnabled
	float		p1[4];			// numDlights, worldBuilt, giRayLength, unused
	float		prevViewProj[16];	// VP of the temporal history image (for reprojection)
	float		prevEye[4];			// eye when the history was written
	float		temporal[4];		// x=temporalEnabled, y=blendAlpha, z=histValid, w=frameCounter
	rtDLight_t	dl[RT_MAX_DLIGHTS];
} rtUBO_t;

// The ray-query / acceleration-structure entry points are EXTENSION functions:
// they exist only once the extensions are enabled at device-creation time, so
// they are deliberately not in qvk.h's "always required" device list.  We
// resolve them here and tolerate their absence (which simply disables RT).
static PFN_vkGetAccelerationStructureBuildSizesKHR		qvkGetAccelerationStructureBuildSizesKHR;
static PFN_vkCreateAccelerationStructureKHR				qvkCreateAccelerationStructureKHR;
static PFN_vkDestroyAccelerationStructureKHR			qvkDestroyAccelerationStructureKHR;
static PFN_vkCmdBuildAccelerationStructuresKHR			qvkCmdBuildAccelerationStructuresKHR;
static PFN_vkGetAccelerationStructureDeviceAddressKHR	qvkGetAccelerationStructureDeviceAddressKHR;

// One world vertex as seen by both the BLAS build (position at offset 0, stride
// 48) and the compute shader (full record via SSBO).  vec4-padded so the GLSL
// std430 layout is trivial (each member 16-aligned).
typedef struct {
	float	pos[4];		// xyz world position (w unused)
	float	nrm[4];		// xyz world normal   (w unused)
	float	rad[4];		// xyz outgoing radiance for indirect hits (w unused)
} rtVertex_t;

// Module state.
typedef struct {
	qboolean	functionsLoaded;	// all optional KHR entry points resolved

	// world acceleration structure (rebuilt per map load)
	qboolean	worldBuilt;
	uint32_t	numVerts;
	uint32_t	numIndices;

	VkBuffer		geoBuf;		VkDeviceMemory geoMem;		// rtVertex_t[]  (host-visible, device addr)
	VkBuffer		idxBuf;		VkDeviceMemory idxMem;		// uint32_t[]    (host-visible, device addr)
	VkBuffer		blasBuf;	VkDeviceMemory blasMem;		// BLAS backing  (device-local)
	VkBuffer		tlasBuf;	VkDeviceMemory tlasMem;		// TLAS backing  (device-local)
	VkAccelerationStructureKHR	blas;
	VkAccelerationStructureKHR	tlas;

	// per-swapchain render targets + compute resources (VK_RT_CreateTargets)
	qboolean		targetsReady;
	VkImage			rtColorImg[VK_NUM_FRAMES];
	VkDeviceMemory	rtColorMem[VK_NUM_FRAMES];
	VkImageView		rtColorView[VK_NUM_FRAMES];
	VkImage			giImg[VK_NUM_FRAMES];				// raw 1-bounce indirect (denoised by blur pass)
	VkDeviceMemory	giMem[VK_NUM_FRAMES];
	VkImageView		giView[VK_NUM_FRAMES];
	VkImageView		depthSampleView[VK_NUM_FRAMES];		// DEPTH-aspect view of vk.depthImage[i]
	VkSampler		colorSampler;						// linear, clamp
	VkSampler		depthSampler;						// nearest, clamp
	VkBuffer		uboBuf[VK_NUM_FRAMES];
	VkDeviceMemory	uboMem[VK_NUM_FRAMES];
	void			*uboMapped[VK_NUM_FRAMES];
	VkDescriptorSetLayout	setLayout;					// lighting pass (8 bindings)
	VkDescriptorSetLayout	blurSetLayout;				// denoise pass (4 bindings)
	VkDescriptorPool		descPool;
	VkDescriptorSet			sets[VK_NUM_FRAMES];
	VkDescriptorSet			blurSets[VK_NUM_FRAMES];
	VkPipelineLayout		pipeLayout;
	VkPipeline				pipe;
	VkPipelineLayout		blurPipeLayout;
	VkPipeline				blurPipe;

	// temporal accumulation: per-frame-slot ping-pong history (race-free: a frame reads
	// the slot's data from 2 frames ago, guaranteed complete by frameFence).  RGBA16F:
	// rgb = accumulated indirect, a = linear view-Z (for disocclusion).
	VkImage			tHistImg[2][VK_NUM_FRAMES];
	VkDeviceMemory	tHistMem[2][VK_NUM_FRAMES];
	VkImageView		tHistView[2][VK_NUM_FRAMES];
	VkImageLayout	tHistLayout[2][VK_NUM_FRAMES];	// tracked (ping images alternate read/write roles)
	uint32_t		tHistPing[VK_NUM_FRAMES];		// which ping holds slot s's last-written data
	qboolean		tHistValid[2][VK_NUM_FRAMES];	// false until first written
	float			tHistVP[2][VK_NUM_FRAMES][16];	// VP used when each ping image was written
	float			tHistEye[2][VK_NUM_FRAMES][3];
	uint32_t		frameCounter;
	VkDescriptorSetLayout	tempSetLayout;
	VkPipelineLayout		tempPipeLayout;
	VkPipeline				tempPipe;
	VkDescriptorSet			tempSets[VK_NUM_FRAMES];

	// camera captured each frame from the 3D view (for depth -> world reconstruction)
	float			camProj[16];
	float			camView[16];
	float			camEye[3];
	float			camVP[16];		// Cz*proj*view this frame (stored as history VP)
	qboolean		camValid;
} vkrt_t;

static vkrt_t	rt;

/*
================
RT_DeviceExtensionAvailable

Local copy of vk_instance.c's helper (which is file-static there) so the RT
module can probe a candidate device before it is selected.
================
*/
static qboolean RT_DeviceExtensionAvailable( VkPhysicalDevice device, const char *name ) {
	VkExtensionProperties	*props;
	uint32_t				count = 0, i;
	qboolean				found = qfalse;

	if ( !qvkEnumerateDeviceExtensionProperties ) {
		return qfalse;
	}
	qvkEnumerateDeviceExtensionProperties( device, NULL, &count, NULL );
	if ( count == 0 ) {
		return qfalse;
	}
	props = (VkExtensionProperties *) ri.Hunk_AllocateTempMemory( sizeof( *props ) * count );
	qvkEnumerateDeviceExtensionProperties( device, NULL, &count, props );
	for ( i = 0; i < count; i++ ) {
		if ( !Q_stricmp( props[i].extensionName, name ) ) {
			found = qtrue;
			break;
		}
	}
	ri.Hunk_FreeTempMemory( props );
	return found;
}

/*
================
RT_AllRequiredExtensions

The trio ray query needs on top of Vulkan 1.3 core: acceleration structure,
ray query, and deferred host operations (a dependency of the former).  Buffer
device address is core in 1.3, so it is requested as a FEATURE, not an
extension.
================
*/
static qboolean RT_AllRequiredExtensions( VkPhysicalDevice phys ) {
	return RT_DeviceExtensionAvailable( phys, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME )
		&& RT_DeviceExtensionAvailable( phys, VK_KHR_RAY_QUERY_EXTENSION_NAME )
		&& RT_DeviceExtensionAvailable( phys, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME );
}

/*
================
VK_RT_DeviceSupported

True when the device exposes the required extensions AND reports the matching
features (rayQuery, accelerationStructure, bufferDeviceAddress).
================
*/
qboolean VK_RT_DeviceSupported( VkPhysicalDevice phys ) {
	VkPhysicalDeviceAccelerationStructureFeaturesKHR	asf;
	VkPhysicalDeviceRayQueryFeaturesKHR					rqf;
	VkPhysicalDeviceBufferDeviceAddressFeatures			bdaf;
	VkPhysicalDeviceFeatures2							features2;
	PFN_vkGetPhysicalDeviceFeatures2					getFeatures2;

	if ( !phys ) {
		return qfalse;
	}
	if ( !RT_AllRequiredExtensions( phys ) ) {
		return qfalse;
	}

	getFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)
		qvkGetInstanceProcAddr( vk.instance, "vkGetPhysicalDeviceFeatures2" );
	if ( !getFeatures2 ) {
		return qfalse;
	}

	memset( &asf, 0, sizeof( asf ) );
	asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	memset( &rqf, 0, sizeof( rqf ) );
	rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	asf.pNext = &rqf;
	memset( &bdaf, 0, sizeof( bdaf ) );
	bdaf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	rqf.pNext = &bdaf;

	memset( &features2, 0, sizeof( features2 ) );
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &asf;

	getFeatures2( phys, &features2 );

	return ( asf.accelerationStructure == VK_TRUE )
		&& ( rqf.rayQuery == VK_TRUE )
		&& ( bdaf.bufferDeviceAddress == VK_TRUE );
}

/*
================
VK_RT_GetRequiredDeviceExtensions
================
*/
void VK_RT_GetRequiredDeviceExtensions( VkPhysicalDevice phys, const char **names, int *count, int maxCount ) {
	static const char *needed[] = {
		VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
		VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
		VK_KHR_RAY_QUERY_EXTENSION_NAME,
	};
	const int neededCount = 3;
	int i;

	for ( i = 0; i < neededCount; i++ ) {
		if ( *count >= maxCount ) {
			break;
		}
		if ( RT_DeviceExtensionAvailable( phys, needed[i] ) ) {
			names[(*count)++] = needed[i];
		}
	}
}

/*
================
VK_RT_BuildDeviceFeatureChain

Initialise the three caller-owned feature structs and splice them onto an
existing pNext chain (tail), returning the new head.  Enables exactly the
features the ray-query path relies on.
================
*/
void *VK_RT_BuildDeviceFeatureChain(
	void *tail,
	VkPhysicalDeviceAccelerationStructureFeaturesKHR *asf,
	VkPhysicalDeviceRayQueryFeaturesKHR *rqf,
	VkPhysicalDeviceBufferDeviceAddressFeatures *bdaf )
{
	memset( asf, 0, sizeof( *asf ) );
	asf->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	asf->accelerationStructure = VK_TRUE;

	memset( rqf, 0, sizeof( *rqf ) );
	rqf->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	rqf->rayQuery = VK_TRUE;

	memset( bdaf, 0, sizeof( *bdaf ) );
	bdaf->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	bdaf->bufferDeviceAddress = VK_TRUE;

	asf->pNext  = rqf;
	rqf->pNext  = bdaf;
	bdaf->pNext = tail;
	return asf;
}

/*
================
RT_LoadFunctions

Resolve the optional KHR entry points.  Returns qfalse (and leaves them NULL) if
any is missing -- which only happens if the extensions were not actually enabled.
================
*/
static qboolean RT_LoadFunctions( void ) {
#define RT_LOAD( name )                                                            \
	q##name = (PFN_##name) qvkGetDeviceProcAddr( vk.device, #name );                \
	if ( !q##name ) { ri.Printf( PRINT_ALL, "...RT: missing %s\n", #name ); return qfalse; }

	RT_LOAD( vkGetAccelerationStructureBuildSizesKHR )
	RT_LOAD( vkCreateAccelerationStructureKHR )
	RT_LOAD( vkDestroyAccelerationStructureKHR )
	RT_LOAD( vkCmdBuildAccelerationStructuresKHR )
	RT_LOAD( vkGetAccelerationStructureDeviceAddressKHR )
#undef RT_LOAD
	return qtrue;
}

//==========================================================================
//
// World acceleration structure build
//
//==========================================================================

/*
================
RT_CreateBuffer

Create a buffer + dedicated memory.  When the usage asks for a device address the
memory is allocated with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT.  Host-visible
buffers are persistently mapped when outMapped is supplied.
================
*/
static VkBuffer RT_CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage,
								 VkMemoryPropertyFlags props, VkDeviceMemory *outMem, void **outMapped ) {
	VkBufferCreateInfo		info;
	VkMemoryRequirements	memReq;
	VkMemoryAllocateInfo	allocInfo;
	VkMemoryAllocateFlagsInfo flagsInfo;
	VkBuffer				buffer;
	VkDeviceMemory			memory;

	memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK( qvkCreateBuffer( vk.device, &info, NULL, &buffer ) );

	qvkGetBufferMemoryRequirements( vk.device, buffer, &memReq );

	memset( &flagsInfo, 0, sizeof( flagsInfo ) );
	flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType( memReq.memoryTypeBits, props );
	if ( usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT ) {
		allocInfo.pNext = &flagsInfo;
	}

	VK_CHECK( qvkAllocateMemory( vk.device, &allocInfo, NULL, &memory ) );
	VK_CHECK( qvkBindBufferMemory( vk.device, buffer, memory, 0 ) );

	if ( outMapped ) {
		VK_CHECK( qvkMapMemory( vk.device, memory, 0, VK_WHOLE_SIZE, 0, outMapped ) );
	}
	*outMem = memory;
	return buffer;
}

static VkDeviceAddress RT_BufAddr( VkBuffer buf ) {
	VkBufferDeviceAddressInfo info;
	memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buf;
	return qvkGetBufferDeviceAddress( vk.device, &info );
}

static void RT_FreeBuffer( VkBuffer *buf, VkDeviceMemory *mem ) {
	if ( *mem )  { qvkFreeMemory( vk.device, *mem, NULL );   *mem = VK_NULL_HANDLE; }
	if ( *buf )  { qvkDestroyBuffer( vk.device, *buf, NULL ); *buf = VK_NULL_HANDLE; }
}

// Transient single-submit command buffer for the one-time AS builds at map load.
static VkCommandBuffer RT_BeginOneShot( void ) {
	VkCommandBufferAllocateInfo	ai;
	VkCommandBufferBeginInfo	bi;
	VkCommandBuffer				cmd;

	memset( &ai, 0, sizeof( ai ) );
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = vk.commandPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VK_CHECK( qvkAllocateCommandBuffers( vk.device, &ai, &cmd ) );

	memset( &bi, 0, sizeof( bi ) );
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK( qvkBeginCommandBuffer( cmd, &bi ) );
	return cmd;
}

static void RT_EndOneShot( VkCommandBuffer cmd ) {
	VkSubmitInfo si;

	VK_CHECK( qvkEndCommandBuffer( cmd ) );
	memset( &si, 0, sizeof( si ) );
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VK_CHECK( qvkQueueSubmit( vk.graphicsQueue, 1, &si, VK_NULL_HANDLE ) );
	qvkQueueWaitIdle( vk.graphicsQueue );
	qvkFreeCommandBuffers( vk.device, vk.commandPool, 1, &cmd );
}

/*
================
RT_ShaderAlbedo

Representative diffuse colour of a surface shader: the average colour of its first
non-lightmap stage image, optionally tinted by a constant-colour stage.  Used as
the outgoing-radiance colour for indirect (colour-bleed) ray hits.
================
*/
static void RT_ShaderAlbedo( const shader_t *sh, float out[3] ) {
	int s;

	out[0] = out[1] = out[2] = 0.5f;	// neutral grey fallback
	if ( !sh ) {
		return;
	}
	for ( s = 0; s < MAX_SHADER_STAGES; s++ ) {
		const shaderStage_t		*st = sh->stages[s];
		const textureBundle_t	*b;
		const image_t			*img;

		if ( !st || !st->active ) {
			continue;
		}
		b = &st->bundle[0];
		if ( b->isLightmap ) {
			continue;
		}
		img = b->image[0];
		if ( !img ) {
			continue;
		}
		out[0] = img->averageColor[0];
		out[1] = img->averageColor[1];
		out[2] = img->averageColor[2];
		if ( st->rgbGen == CGEN_CONST ) {
			out[0] *= st->constantColor[0] / 255.0f;
			out[1] *= st->constantColor[1] / 255.0f;
			out[2] *= st->constantColor[2] / 255.0f;
		}
		return;
	}
}

// Decide whether a world surface contributes to the ray-tracing geometry, and
// hand back its concrete surface type.  Skip sky, nodraw, flares and non-geometry.
static qboolean RT_SurfaceUsable( const msurface_t *surf, int *typeOut ) {
	int type;

	if ( !surf || !surf->data || !surf->shader ) {
		return qfalse;
	}
	type = *(const surfaceType_t *)surf->data;
	if ( type != SF_FACE && type != SF_GRID && type != SF_TRIANGLES ) {
		return qfalse;
	}
	if ( surf->shader->isSky ) {
		return qfalse;
	}
	if ( surf->shader->surfaceFlags & ( SURF_NODRAW | SURF_SKY ) ) {
		return qfalse;
	}
	*typeOut = type;
	return qtrue;
}

// Count the vertices/indices a usable surface contributes.
static void RT_SurfaceCounts( const msurface_t *surf, int type, int *nv, int *ni ) {
	switch ( type ) {
	case SF_FACE: {
		const srfSurfaceFace_t *f = (const srfSurfaceFace_t *)surf->data;
		*nv = f->numPoints;
		*ni = f->numIndices;
		break;
	}
	case SF_TRIANGLES: {
		const srfTriangles_t *t = (const srfTriangles_t *)surf->data;
		*nv = t->numVerts;
		*ni = t->numIndexes;
		break;
	}
	case SF_GRID: {
		const srfGridMesh_t *g = (const srfGridMesh_t *)surf->data;
		*nv = g->width * g->height;
		*ni = ( g->width - 1 ) * ( g->height - 1 ) * 6;
		break;
	}
	default:
		*nv = *ni = 0;
		break;
	}
}

// Bilinear sample of a retained 128x128 lightmap (overbright-shifted, 0..255) into
// a 0..1 RGB triple.  Falls back to white when no lightmap data is available.
#define RT_LM_SIZE	128
static void RT_SampleLightmap( int lmIndex, float u, float v, float out[3] ) {
	const byte	*lm;
	float		fx, fy, dx, dy;
	int			x0, y0, x1, y1, c;

	out[0] = out[1] = out[2] = 1.0f;
	if ( !tr.rtLightmapData || lmIndex < 0 || lmIndex >= tr.numLightmaps ) {
		return;
	}
	lm = tr.rtLightmapData + (size_t)lmIndex * RT_LM_SIZE * RT_LM_SIZE * 4;
	u = u < 0.0f ? 0.0f : ( u > 1.0f ? 1.0f : u );
	v = v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
	fx = u * ( RT_LM_SIZE - 1 );
	fy = v * ( RT_LM_SIZE - 1 );
	x0 = (int)fx; y0 = (int)fy;
	x1 = ( x0 < RT_LM_SIZE - 1 ) ? x0 + 1 : x0;
	y1 = ( y0 < RT_LM_SIZE - 1 ) ? y0 + 1 : y0;
	dx = fx - x0; dy = fy - y0;
	for ( c = 0; c < 3; c++ ) {
		float a  = lm[ ( y0 * RT_LM_SIZE + x0 ) * 4 + c ];
		float b  = lm[ ( y0 * RT_LM_SIZE + x1 ) * 4 + c ];
		float cc = lm[ ( y1 * RT_LM_SIZE + x0 ) * 4 + c ];
		float d  = lm[ ( y1 * RT_LM_SIZE + x1 ) * 4 + c ];
		float top = a + ( b - a ) * dx;
		float bot = cc + ( d - cc ) * dx;
		out[c] = ( top + ( bot - top ) * dy ) / 255.0f;
	}
}

// outgoing radiance for a vertex = surface albedo * baked light at that vertex
// (lightmap sample when the surface is lightmapped, else its vertex colour).
static void RT_VertexRadiance( const float albedo[3], int lmIndex,
							   float lu, float lv, const byte *vcolor, float rad[3] ) {
	float light[3];
	if ( lmIndex >= 0 ) {
		RT_SampleLightmap( lmIndex, lu, lv, light );
	} else if ( vcolor ) {
		light[0] = vcolor[0] / 255.0f;
		light[1] = vcolor[1] / 255.0f;
		light[2] = vcolor[2] / 255.0f;
	} else {
		light[0] = light[1] = light[2] = 1.0f;
	}
	rad[0] = albedo[0] * light[0];
	rad[1] = albedo[1] * light[1];
	rad[2] = albedo[2] * light[2];
}

// Append one usable surface's geometry into the flat vertex/index arrays.
// vbase is the running vertex count (added to local indices); both *vcount and
// *icount are advanced.
static void RT_EmitSurface( const msurface_t *surf, int type,
							rtVertex_t *verts, uint32_t *indices,
							uint32_t *vcount, uint32_t *icount ) {
	float	albedo[3];
	int		lmIndex = surf->shader->lightmapIndex;
	uint32_t vbase = *vcount;
	int		i;

	RT_ShaderAlbedo( surf->shader, albedo );

	switch ( type ) {
	case SF_FACE: {
		const srfSurfaceFace_t *f = (const srfSurfaceFace_t *)surf->data;
		const int *idx = (const int *)( (const byte *)f + f->ofsIndices );
		for ( i = 0; i < f->numPoints; i++ ) {
			rtVertex_t *v = &verts[ *vcount + i ];
			const byte *vcol = (const byte *)&f->points[i][7];	// packed RGBA
			v->pos[0] = f->points[i][0]; v->pos[1] = f->points[i][1]; v->pos[2] = f->points[i][2]; v->pos[3] = 0;
			v->nrm[0] = f->plane.normal[0]; v->nrm[1] = f->plane.normal[1]; v->nrm[2] = f->plane.normal[2]; v->nrm[3] = 0;
			RT_VertexRadiance( albedo, lmIndex, f->points[i][5], f->points[i][6], vcol, v->rad );
			v->rad[3] = 0;
		}
		for ( i = 0; i < f->numIndices; i++ ) {
			indices[ *icount + i ] = vbase + (uint32_t)idx[i];
		}
		*vcount += f->numPoints;
		*icount += f->numIndices;
		break;
	}
	case SF_TRIANGLES: {
		const srfTriangles_t *t = (const srfTriangles_t *)surf->data;
		for ( i = 0; i < t->numVerts; i++ ) {
			rtVertex_t *v = &verts[ *vcount + i ];
			const drawVert_t *dv = &t->verts[i];
			v->pos[0] = dv->xyz[0]; v->pos[1] = dv->xyz[1]; v->pos[2] = dv->xyz[2]; v->pos[3] = 0;
			v->nrm[0] = dv->normal[0]; v->nrm[1] = dv->normal[1]; v->nrm[2] = dv->normal[2]; v->nrm[3] = 0;
			RT_VertexRadiance( albedo, lmIndex, dv->lightmap[0], dv->lightmap[1], dv->color, v->rad );
			v->rad[3] = 0;
		}
		for ( i = 0; i < t->numIndexes; i++ ) {
			indices[ *icount + i ] = vbase + (uint32_t)t->indexes[i];
		}
		*vcount += t->numVerts;
		*icount += t->numIndexes;
		break;
	}
	case SF_GRID: {
		const srfGridMesh_t *g = (const srfGridMesh_t *)surf->data;
		int r, c, w = g->width, h = g->height;
		for ( i = 0; i < w * h; i++ ) {
			rtVertex_t *v = &verts[ *vcount + i ];
			const drawVert_t *dv = &g->verts[i];
			v->pos[0] = dv->xyz[0]; v->pos[1] = dv->xyz[1]; v->pos[2] = dv->xyz[2]; v->pos[3] = 0;
			v->nrm[0] = dv->normal[0]; v->nrm[1] = dv->normal[1]; v->nrm[2] = dv->normal[2]; v->nrm[3] = 0;
			RT_VertexRadiance( albedo, lmIndex, dv->lightmap[0], dv->lightmap[1], dv->color, v->rad );
			v->rad[3] = 0;
		}
		for ( r = 0; r < h - 1; r++ ) {
			for ( c = 0; c < w - 1; c++ ) {
				uint32_t v00 = vbase + (uint32_t)( r * w + c );
				uint32_t v01 = v00 + 1;
				uint32_t v10 = vbase + (uint32_t)( ( r + 1 ) * w + c );
				uint32_t v11 = v10 + 1;
				indices[ (*icount)++ ] = v00; indices[ (*icount)++ ] = v10; indices[ (*icount)++ ] = v01;
				indices[ (*icount)++ ] = v01; indices[ (*icount)++ ] = v10; indices[ (*icount)++ ] = v11;
			}
		}
		*vcount += w * h;
		break;
	}
	default:
		break;
	}
}

/*
================
VK_RT_FreeWorld
================
*/
void VK_RT_FreeWorld( void ) {
	if ( !vk.device ) {
		return;
	}
	if ( rt.tlas && qvkDestroyAccelerationStructureKHR ) {
		qvkDestroyAccelerationStructureKHR( vk.device, rt.tlas, NULL );
		rt.tlas = VK_NULL_HANDLE;
	}
	if ( rt.blas && qvkDestroyAccelerationStructureKHR ) {
		qvkDestroyAccelerationStructureKHR( vk.device, rt.blas, NULL );
		rt.blas = VK_NULL_HANDLE;
	}
	RT_FreeBuffer( &rt.tlasBuf, &rt.tlasMem );
	RT_FreeBuffer( &rt.blasBuf, &rt.blasMem );
	RT_FreeBuffer( &rt.idxBuf, &rt.idxMem );
	RT_FreeBuffer( &rt.geoBuf, &rt.geoMem );
	rt.numVerts = rt.numIndices = 0;
	rt.worldBuilt = qfalse;
}

/*
================
VK_RT_BuildWorld

Walk tr.world's static surfaces, pack them into one device-addressable geometry
buffer, and build a BLAS + single-instance TLAS from it.  Called at the end of
RE_LoadWorldMap; a no-op unless the Vulkan RT path is active.
================
*/
void VK_RT_BuildWorld( void ) {
	const VkMemoryPropertyFlags hostProps   = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkMemoryPropertyFlags deviceProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	int			totalVerts = 0, totalIndices = 0;
	int			i;
	rtVertex_t	*verts;
	uint32_t	*indices;
	uint32_t	vcount = 0, icount = 0;
	void		*geoMapped, *idxMapped;

	VkAccelerationStructureGeometryKHR			blasGeom, tlasGeom;
	VkAccelerationStructureBuildGeometryInfoKHR	blasBuild, tlasBuild;
	VkAccelerationStructureBuildSizesInfoKHR	blasSizes, tlasSizes;
	VkAccelerationStructureCreateInfoKHR		asCreate;
	VkAccelerationStructureBuildRangeInfoKHR	blasRange, tlasRange;
	const VkAccelerationStructureBuildRangeInfoKHR *pBlasRange, *pTlasRange;
	VkAccelerationStructureDeviceAddressInfoKHR	addrInfo;
	VkDeviceAddress								blasAddr;
	VkBuffer		blasScratch = VK_NULL_HANDLE, tlasScratch = VK_NULL_HANDLE, instBuf = VK_NULL_HANDLE;
	VkDeviceMemory	blasScratchMem = VK_NULL_HANDLE, tlasScratchMem = VK_NULL_HANDLE, instMem = VK_NULL_HANDLE;
	void			*instMapped;
	VkAccelerationStructureInstanceKHR			inst;
	uint32_t		blasPrims, tlasPrims = 1;
	VkMemoryBarrier	memBarrier;
	VkCommandBuffer	cmd;

	if ( !VK_RT_Active() || !tr.world ) {
		return;
	}

	// release any previous map's structures (map change without device teardown)
	VK_RT_FreeWorld();

	// --- pass 1: count usable geometry ---
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		int type, nv, ni;
		if ( !RT_SurfaceUsable( &tr.world->surfaces[i], &type ) ) {
			continue;
		}
		RT_SurfaceCounts( &tr.world->surfaces[i], type, &nv, &ni );
		totalVerts += nv;
		totalIndices += ni;
	}
	if ( totalVerts == 0 || totalIndices == 0 ) {
		ri.Printf( PRINT_ALL, "...RT: world has no usable geometry; ray tracing inactive this map\n" );
		return;
	}

	// --- pass 2: pack into temp arrays ---
	verts = (rtVertex_t *) ri.Hunk_AllocateTempMemory( sizeof( rtVertex_t ) * totalVerts );
	indices = (uint32_t *) ri.Hunk_AllocateTempMemory( sizeof( uint32_t ) * totalIndices );
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		int type;
		if ( !RT_SurfaceUsable( &tr.world->surfaces[i], &type ) ) {
			continue;
		}
		RT_EmitSurface( &tr.world->surfaces[i], type, verts, indices, &vcount, &icount );
	}
	rt.numVerts = vcount;
	rt.numIndices = icount;

	// --- upload geometry into host-visible, device-addressable buffers ---
	rt.geoBuf = RT_CreateBuffer( sizeof( rtVertex_t ) * vcount,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		hostProps, &rt.geoMem, &geoMapped );
	Com_Memcpy( geoMapped, verts, sizeof( rtVertex_t ) * vcount );

	rt.idxBuf = RT_CreateBuffer( sizeof( uint32_t ) * icount,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		hostProps, &rt.idxMem, &idxMapped );
	Com_Memcpy( idxMapped, indices, sizeof( uint32_t ) * icount );

	ri.Hunk_FreeTempMemory( indices );
	ri.Hunk_FreeTempMemory( verts );

	// --- BLAS geometry description ---
	blasPrims = icount / 3;
	memset( &blasGeom, 0, sizeof( blasGeom ) );
	blasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	blasGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	blasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	blasGeom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	blasGeom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	blasGeom.geometry.triangles.vertexData.deviceAddress = RT_BufAddr( rt.geoBuf );
	blasGeom.geometry.triangles.vertexStride = sizeof( rtVertex_t );
	blasGeom.geometry.triangles.maxVertex = vcount - 1;
	blasGeom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	blasGeom.geometry.triangles.indexData.deviceAddress = RT_BufAddr( rt.idxBuf );

	memset( &blasBuild, 0, sizeof( blasBuild ) );
	blasBuild.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	blasBuild.geometryCount = 1;
	blasBuild.pGeometries = &blasGeom;

	memset( &blasSizes, 0, sizeof( blasSizes ) );
	blasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	qvkGetAccelerationStructureBuildSizesKHR( vk.device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blasBuild, &blasPrims, &blasSizes );

	rt.blasBuf = RT_CreateBuffer( blasSizes.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		deviceProps, &rt.blasMem, NULL );

	memset( &asCreate, 0, sizeof( asCreate ) );
	asCreate.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asCreate.buffer = rt.blasBuf;
	asCreate.size = blasSizes.accelerationStructureSize;
	asCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	VK_CHECK( qvkCreateAccelerationStructureKHR( vk.device, &asCreate, NULL, &rt.blas ) );

	blasScratch = RT_CreateBuffer( blasSizes.buildScratchSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		deviceProps, &blasScratchMem, NULL );

	blasBuild.dstAccelerationStructure = rt.blas;
	blasBuild.scratchData.deviceAddress = RT_BufAddr( blasScratch );

	memset( &blasRange, 0, sizeof( blasRange ) );
	blasRange.primitiveCount = blasPrims;
	pBlasRange = &blasRange;

	// --- BLAS device address (referenced by the TLAS instance) ---
	memset( &addrInfo, 0, sizeof( addrInfo ) );
	addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addrInfo.accelerationStructure = rt.blas;
	blasAddr = qvkGetAccelerationStructureDeviceAddressKHR( vk.device, &addrInfo );

	// --- single identity instance for the TLAS ---
	memset( &inst, 0, sizeof( inst ) );
	inst.transform.matrix[0][0] = 1.0f;
	inst.transform.matrix[1][1] = 1.0f;
	inst.transform.matrix[2][2] = 1.0f;
	inst.mask = 0xFF;
	inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	inst.accelerationStructureReference = blasAddr;

	instBuf = RT_CreateBuffer( sizeof( inst ),
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
		hostProps, &instMem, &instMapped );
	Com_Memcpy( instMapped, &inst, sizeof( inst ) );

	// --- TLAS geometry description ---
	memset( &tlasGeom, 0, sizeof( tlasGeom ) );
	tlasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	tlasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	tlasGeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
	tlasGeom.geometry.instances.data.deviceAddress = RT_BufAddr( instBuf );

	memset( &tlasBuild, 0, sizeof( tlasBuild ) );
	tlasBuild.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	tlasBuild.geometryCount = 1;
	tlasBuild.pGeometries = &tlasGeom;

	memset( &tlasSizes, 0, sizeof( tlasSizes ) );
	tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	qvkGetAccelerationStructureBuildSizesKHR( vk.device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild, &tlasPrims, &tlasSizes );

	rt.tlasBuf = RT_CreateBuffer( tlasSizes.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		deviceProps, &rt.tlasMem, NULL );

	memset( &asCreate, 0, sizeof( asCreate ) );
	asCreate.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asCreate.buffer = rt.tlasBuf;
	asCreate.size = tlasSizes.accelerationStructureSize;
	asCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	VK_CHECK( qvkCreateAccelerationStructureKHR( vk.device, &asCreate, NULL, &rt.tlas ) );

	tlasScratch = RT_CreateBuffer( tlasSizes.buildScratchSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		deviceProps, &tlasScratchMem, NULL );

	tlasBuild.dstAccelerationStructure = rt.tlas;
	tlasBuild.scratchData.deviceAddress = RT_BufAddr( tlasScratch );

	memset( &tlasRange, 0, sizeof( tlasRange ) );
	tlasRange.primitiveCount = 1;
	pTlasRange = &tlasRange;

	// --- record both builds (BLAS, barrier, TLAS) ---
	cmd = RT_BeginOneShot();
	qvkCmdBuildAccelerationStructuresKHR( cmd, 1, &blasBuild, &pBlasRange );

	memset( &memBarrier, 0, sizeof( memBarrier ) );
	memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	memBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	qvkCmdPipelineBarrier( cmd,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		0, 1, &memBarrier, 0, NULL, 0, NULL );

	qvkCmdBuildAccelerationStructuresKHR( cmd, 1, &tlasBuild, &pTlasRange );
	RT_EndOneShot( cmd );

	// scratch + instance staging are only needed during the build
	RT_FreeBuffer( &tlasScratch, &tlasScratchMem );
	RT_FreeBuffer( &blasScratch, &blasScratchMem );
	RT_FreeBuffer( &instBuf, &instMem );

	rt.worldBuilt = qtrue;
	ri.Printf( PRINT_ALL, "...RT: world AS built (%u verts, %u tris)\n", rt.numVerts, rt.numIndices / 3 );
}

/*
================
VK_RT_Init
================
*/
void VK_RT_Init( void ) {
	memset( &rt, 0, sizeof( rt ) );

	if ( !vk.rtxEnabled ) {
		return;		// disabled by cvar or unsupported device -- nothing to do
	}

	if ( !RT_LoadFunctions() ) {
		ri.Printf( PRINT_ALL, "...ray tracing: entry points unavailable, disabling\n" );
		vk.rtxEnabled = qfalse;
		return;
	}
	rt.functionsLoaded = qtrue;

	ri.Printf( PRINT_ALL, "...ray tracing: enabled (%s)\n", vk.devProps.deviceName );
}

/*
================
VK_RT_Shutdown
================
*/
void VK_RT_Shutdown( void ) {
	VK_RT_FreeWorld();
	VK_RT_DestroyTargets();		// free compute targets/pipeline BEFORE the rt memset below
								// (otherwise their handles are zeroed and leak on device destroy)
	qvkGetAccelerationStructureBuildSizesKHR = NULL;
	qvkCreateAccelerationStructureKHR = NULL;
	qvkDestroyAccelerationStructureKHR = NULL;
	qvkCmdBuildAccelerationStructuresKHR = NULL;
	qvkGetAccelerationStructureDeviceAddressKHR = NULL;
	memset( &rt, 0, sizeof( rt ) );
}

/*
================
VK_RT_Active
================
*/
qboolean VK_RT_Active( void ) {
	return ( vk.rtxEnabled && rt.functionsLoaded );
}

// VK-type-free wrapper for the shared (GL+VK) draw path: true once the world AS is
// up and the deferred RT lighting is actually running, so tr_shade.c suppresses its
// own additive dynamic-light pass (the RT pass relights dynamic lights, with shadows).
qboolean R_RaytracingActive( void ) {
	return ( VK_RT_Active() && rt.worldBuilt );
}

//==========================================================================
//
// Deferred lighting resolve (compute pass + blit to swapchain)
//
//==========================================================================

static void RT_ImageBarrier( VkImage image, VkImageAspectFlags aspect,
	VkImageLayout oldLayout, VkImageLayout newLayout,
	VkAccessFlags srcAccess, VkAccessFlags dstAccess,
	VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage )
{
	VkImageMemoryBarrier b;
	memset( &b, 0, sizeof( b ) );
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.oldLayout = oldLayout;
	b.newLayout = newLayout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange.aspectMask = aspect;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	qvkCmdPipelineBarrier( vk.cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b );
}

//
// column-major 4x4 helpers (OpenGL/Q3 convention)
//
static void RT_Mat4Mul( const float *a, const float *b, float *out ) {
	int c, r, k;
	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			float s = 0.0f;
			for ( k = 0; k < 4; k++ ) {
				s += a[k * 4 + r] * b[c * 4 + k];
			}
			out[c * 4 + r] = s;
		}
	}
}

// general 4x4 inverse (cofactor method); returns qfalse if singular
static qboolean RT_Mat4Inverse( const float *m, float *inv ) {
	float det;
	int i;

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

	det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if ( det == 0.0f ) {
		return qfalse;
	}
	det = 1.0f / det;
	for ( i = 0; i < 16; i++ ) {
		inv[i] *= det;
	}
	return qtrue;
}

/*
================
VK_RT_SetCamera
================
*/
void VK_RT_SetCamera( const float *projMatrix, const float *viewMatrix, const float *eye ) {
	Com_Memcpy( rt.camProj, projMatrix, sizeof( rt.camProj ) );
	Com_Memcpy( rt.camView, viewMatrix, sizeof( rt.camView ) );
	rt.camEye[0] = eye[0];
	rt.camEye[1] = eye[1];
	rt.camEye[2] = eye[2];
	rt.camValid = qtrue;
}

//==========================================================================
//
// Compute targets, descriptors and pipeline
//
//==========================================================================

static VkImageView RT_DepthAspectView( VkImage img ) {
	VkImageViewCreateInfo vi;
	VkImageView view;
	memset( &vi, 0, sizeof( vi ) );
	vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image = img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = vk.depthFormat;
	vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;	// sample depth only
	vi.subresourceRange.levelCount = 1;
	vi.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &vi, NULL, &view ) );
	return view;
}

static VkSampler RT_CreateSampler( VkFilter filter ) {
	VkSamplerCreateInfo si;
	VkSampler s;
	memset( &si, 0, sizeof( si ) );
	si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	si.magFilter = filter;
	si.minFilter = filter;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.maxLod = 0.0f;
	VK_CHECK( qvkCreateSampler( vk.device, &si, NULL, &s ) );
	return s;
}

static VkPipeline RT_BuildComputePipe( VkPipelineLayout layout, const uint32_t *spv, size_t spvSize ) {
	VkShaderModuleCreateInfo	smi;
	VkShaderModule				module;
	VkComputePipelineCreateInfo	cpi;
	VkPipeline					pipe;

	memset( &smi, 0, sizeof( smi ) );
	smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smi.codeSize = spvSize;
	smi.pCode = spv;
	VK_CHECK( qvkCreateShaderModule( vk.device, &smi, NULL, &module ) );

	memset( &cpi, 0, sizeof( cpi ) );
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = module;
	cpi.stage.pName = "main";
	cpi.layout = layout;
	VK_CHECK( qvkCreateComputePipelines( vk.device, vk.pipelineCache, 1, &cpi, NULL, &pipe ) );

	qvkDestroyShaderModule( vk.device, module, NULL );
	return pipe;
}

static qboolean RT_CreateComputePipeline( void ) {
	VkDescriptorSetLayoutBinding	b[9];
	VkDescriptorSetLayoutCreateInfo	li;
	VkPipelineLayoutCreateInfo		pli;
	int i;

	// --- lighting pass: 9 bindings ---
	memset( b, 0, sizeof( b ) );
	for ( i = 0; i < 9; i++ ) {
		b[i].binding = i;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;		// scene colour
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;		// depth
	b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;				// lit output (base*ambient+direct)
	b[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;	// TLAS
	b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;				// geometry
	b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;				// indices
	b[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;				// per-frame UBO
	b[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;				// raw indirect output
	b[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;		// albedo G-buffer

	memset( &li, 0, sizeof( li ) );
	li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	li.bindingCount = 9;
	li.pBindings = b;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &li, NULL, &rt.setLayout ) );

	memset( &pli, 0, sizeof( pli ) );
	pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &rt.setLayout;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &pli, NULL, &rt.pipeLayout ) );
	rt.pipe = RT_BuildComputePipe( rt.pipeLayout, vk_spv_rt_light_comp, sizeof( vk_spv_rt_light_comp ) );

	// --- denoise pass: 4 bindings (gi sampler, depth sampler, out storage, UBO) ---
	memset( b, 0, sizeof( b ) );
	for ( i = 0; i < 4; i++ ) {
		b[i].binding = i;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;		// raw indirect
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;		// depth
	b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;				// out (read+write)
	b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;				// per-frame UBO

	li.bindingCount = 4;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &li, NULL, &rt.blurSetLayout ) );

	pli.pSetLayouts = &rt.blurSetLayout;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &pli, NULL, &rt.blurPipeLayout ) );
	rt.blurPipe = RT_BuildComputePipe( rt.blurPipeLayout, vk_spv_rt_blur_comp, sizeof( vk_spv_rt_blur_comp ) );

	// --- temporal pass: 5 bindings (gi storage RW, depth, history read, history write, UBO) ---
	memset( b, 0, sizeof( b ) );
	for ( i = 0; i < 5; i++ ) {
		b[i].binding = i;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;			// current indirect (read+write)
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;	// depth
	b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;	// history read
	b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;			// history write
	b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;			// UBO

	li.bindingCount = 5;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &li, NULL, &rt.tempSetLayout ) );

	pli.pSetLayouts = &rt.tempSetLayout;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &pli, NULL, &rt.tempPipeLayout ) );
	rt.tempPipe = RT_BuildComputePipe( rt.tempPipeLayout, vk_spv_rt_temporal_comp, sizeof( vk_spv_rt_temporal_comp ) );

	return qtrue;
}

/*
================
VK_RT_CreateTargets

Allocate the per-frame rtColor storage image, depth-sample views, samplers,
per-frame UBOs and the compute descriptor pool/sets + pipeline.  No-op unless RT
is active.  Descriptor sets are fully (re)written each frame in VK_RT_Resolve, so
nothing here depends on the (later-built) world acceleration structure.
================
*/
qboolean VK_RT_CreateTargets( void ) {
	const VkMemoryPropertyFlags devProps  = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	const VkMemoryPropertyFlags hostProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkDescriptorPoolSize	poolSizes[5];
	VkDescriptorPoolCreateInfo	dpi;
	VkDescriptorSetAllocateInfo	dsai;
	VkDescriptorSetLayout	layouts[VK_NUM_FRAMES];
	int i;

	if ( !VK_RT_Active() ) {
		return qtrue;
	}

	rt.colorSampler = RT_CreateSampler( VK_FILTER_LINEAR );
	rt.depthSampler = RT_CreateSampler( VK_FILTER_NEAREST );

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		VkImageCreateInfo		ii;
		VkMemoryRequirements	mr;
		VkMemoryAllocateInfo	ai;
		VkImageViewCreateInfo	vi;

		// rtColor: HDR storage image the compute pass writes, then we blit to swapchain
		memset( &ii, 0, sizeof( ii ) );
		ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		ii.extent.width = vk.renderExtent.width;
		ii.extent.height = vk.renderExtent.height;
		ii.extent.depth = 1;
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VK_CHECK( qvkCreateImage( vk.device, &ii, NULL, &rt.rtColorImg[i] ) );

		qvkGetImageMemoryRequirements( vk.device, rt.rtColorImg[i], &mr );
		memset( &ai, 0, sizeof( ai ) );
		ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = VK_FindMemoryType( mr.memoryTypeBits, devProps );
		VK_CHECK( qvkAllocateMemory( vk.device, &ai, NULL, &rt.rtColorMem[i] ) );
		VK_CHECK( qvkBindImageMemory( vk.device, rt.rtColorImg[i], rt.rtColorMem[i], 0 ) );

		memset( &vi, 0, sizeof( vi ) );
		vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image = rt.rtColorImg[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = 1;
		vi.subresourceRange.layerCount = 1;
		VK_CHECK( qvkCreateImageView( vk.device, &vi, NULL, &rt.rtColorView[i] ) );

		// giImage: raw 1-bounce indirect, written by the lighting pass, sampled+denoised
		// by the blur pass (so STORAGE + SAMPLED).
		ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VK_CHECK( qvkCreateImage( vk.device, &ii, NULL, &rt.giImg[i] ) );
		qvkGetImageMemoryRequirements( vk.device, rt.giImg[i], &mr );
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = VK_FindMemoryType( mr.memoryTypeBits, devProps );
		VK_CHECK( qvkAllocateMemory( vk.device, &ai, NULL, &rt.giMem[i] ) );
		VK_CHECK( qvkBindImageMemory( vk.device, rt.giImg[i], rt.giMem[i], 0 ) );
		vi.image = rt.giImg[i];
		VK_CHECK( qvkCreateImageView( vk.device, &vi, NULL, &rt.giView[i] ) );

		// temporal history: ping-pong pair per frame slot (same R16F storage+sampled format)
		{
			int p;
			for ( p = 0; p < 2; p++ ) {
				VK_CHECK( qvkCreateImage( vk.device, &ii, NULL, &rt.tHistImg[p][i] ) );
				qvkGetImageMemoryRequirements( vk.device, rt.tHistImg[p][i], &mr );
				ai.allocationSize = mr.size;
				ai.memoryTypeIndex = VK_FindMemoryType( mr.memoryTypeBits, devProps );
				VK_CHECK( qvkAllocateMemory( vk.device, &ai, NULL, &rt.tHistMem[p][i] ) );
				VK_CHECK( qvkBindImageMemory( vk.device, rt.tHistImg[p][i], rt.tHistMem[p][i], 0 ) );
				vi.image = rt.tHistImg[p][i];
				VK_CHECK( qvkCreateImageView( vk.device, &vi, NULL, &rt.tHistView[p][i] ) );
				rt.tHistLayout[p][i] = VK_IMAGE_LAYOUT_UNDEFINED;
				rt.tHistValid[p][i] = qfalse;
			}
			rt.tHistPing[i] = 0;
		}

		rt.depthSampleView[i] = RT_DepthAspectView( vk.depthImage[i] );

		rt.uboBuf[i] = RT_CreateBuffer( sizeof( rtUBO_t ), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			hostProps, &rt.uboMem[i], &rt.uboMapped[i] );
	}

	// descriptor pool covering the lighting, temporal and denoise sets per frame.  Per frame:
	// combined samplers = 3 (light: scene,depth,albedo) + 2 (temporal: depth,histRead) + 2 (blur: gi,depth) = 7;
	// storage images   = 2 (light: out,giOut) + 2 (temporal: gi,histWrite) + 1 (blur: out) = 5;
	// accel struct = 1 (light); storage buffers = 2 (light: geo,idx); uniform buffers = 3 (light,temporal,blur).
	memset( poolSizes, 0, sizeof( poolSizes ) );
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;	poolSizes[0].descriptorCount = 7 * VK_NUM_FRAMES;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;			poolSizes[1].descriptorCount = 5 * VK_NUM_FRAMES;
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; poolSizes[2].descriptorCount = 1 * VK_NUM_FRAMES;
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;			poolSizes[3].descriptorCount = 2 * VK_NUM_FRAMES;
	poolSizes[4].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;			poolSizes[4].descriptorCount = 3 * VK_NUM_FRAMES;

	memset( &dpi, 0, sizeof( dpi ) );
	dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpi.maxSets = 3 * VK_NUM_FRAMES;
	dpi.poolSizeCount = 5;
	dpi.pPoolSizes = poolSizes;
	VK_CHECK( qvkCreateDescriptorPool( vk.device, &dpi, NULL, &rt.descPool ) );

	if ( !RT_CreateComputePipeline() ) {
		return qfalse;
	}

	memset( &dsai, 0, sizeof( dsai ) );
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = rt.descPool;
	dsai.descriptorSetCount = VK_NUM_FRAMES;

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) { layouts[i] = rt.setLayout; }
	dsai.pSetLayouts = layouts;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &dsai, rt.sets ) );

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) { layouts[i] = rt.blurSetLayout; }
	dsai.pSetLayouts = layouts;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &dsai, rt.blurSets ) );

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) { layouts[i] = rt.tempSetLayout; }
	dsai.pSetLayouts = layouts;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &dsai, rt.tempSets ) );

	rt.targetsReady = qtrue;
	return qtrue;
}

/*
================
VK_RT_DestroyTargets
================
*/
void VK_RT_DestroyTargets( void ) {
	int i;

	if ( !vk.device ) {
		return;
	}
	if ( rt.pipe )           { qvkDestroyPipeline( vk.device, rt.pipe, NULL ); rt.pipe = VK_NULL_HANDLE; }
	if ( rt.blurPipe )       { qvkDestroyPipeline( vk.device, rt.blurPipe, NULL ); rt.blurPipe = VK_NULL_HANDLE; }
	if ( rt.tempPipe )       { qvkDestroyPipeline( vk.device, rt.tempPipe, NULL ); rt.tempPipe = VK_NULL_HANDLE; }
	if ( rt.pipeLayout )     { qvkDestroyPipelineLayout( vk.device, rt.pipeLayout, NULL ); rt.pipeLayout = VK_NULL_HANDLE; }
	if ( rt.blurPipeLayout ) { qvkDestroyPipelineLayout( vk.device, rt.blurPipeLayout, NULL ); rt.blurPipeLayout = VK_NULL_HANDLE; }
	if ( rt.tempPipeLayout ) { qvkDestroyPipelineLayout( vk.device, rt.tempPipeLayout, NULL ); rt.tempPipeLayout = VK_NULL_HANDLE; }
	if ( rt.setLayout )      { qvkDestroyDescriptorSetLayout( vk.device, rt.setLayout, NULL ); rt.setLayout = VK_NULL_HANDLE; }
	if ( rt.blurSetLayout )  { qvkDestroyDescriptorSetLayout( vk.device, rt.blurSetLayout, NULL ); rt.blurSetLayout = VK_NULL_HANDLE; }
	if ( rt.tempSetLayout )  { qvkDestroyDescriptorSetLayout( vk.device, rt.tempSetLayout, NULL ); rt.tempSetLayout = VK_NULL_HANDLE; }
	if ( rt.descPool )       { qvkDestroyDescriptorPool( vk.device, rt.descPool, NULL ); rt.descPool = VK_NULL_HANDLE; }

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		int p;
		rt.sets[i] = VK_NULL_HANDLE;
		rt.blurSets[i] = VK_NULL_HANDLE;
		rt.tempSets[i] = VK_NULL_HANDLE;
		for ( p = 0; p < 2; p++ ) {
			if ( rt.tHistView[p][i] ) { qvkDestroyImageView( vk.device, rt.tHistView[p][i], NULL ); rt.tHistView[p][i] = VK_NULL_HANDLE; }
			if ( rt.tHistImg[p][i] )  { qvkDestroyImage( vk.device, rt.tHistImg[p][i], NULL ); rt.tHistImg[p][i] = VK_NULL_HANDLE; }
			if ( rt.tHistMem[p][i] )  { qvkFreeMemory( vk.device, rt.tHistMem[p][i], NULL ); rt.tHistMem[p][i] = VK_NULL_HANDLE; }
		}
		if ( rt.rtColorView[i] )     { qvkDestroyImageView( vk.device, rt.rtColorView[i], NULL ); rt.rtColorView[i] = VK_NULL_HANDLE; }
		if ( rt.rtColorImg[i] )      { qvkDestroyImage( vk.device, rt.rtColorImg[i], NULL ); rt.rtColorImg[i] = VK_NULL_HANDLE; }
		if ( rt.rtColorMem[i] )      { qvkFreeMemory( vk.device, rt.rtColorMem[i], NULL ); rt.rtColorMem[i] = VK_NULL_HANDLE; }
		if ( rt.giView[i] )          { qvkDestroyImageView( vk.device, rt.giView[i], NULL ); rt.giView[i] = VK_NULL_HANDLE; }
		if ( rt.giImg[i] )           { qvkDestroyImage( vk.device, rt.giImg[i], NULL ); rt.giImg[i] = VK_NULL_HANDLE; }
		if ( rt.giMem[i] )           { qvkFreeMemory( vk.device, rt.giMem[i], NULL ); rt.giMem[i] = VK_NULL_HANDLE; }
		if ( rt.depthSampleView[i] ) { qvkDestroyImageView( vk.device, rt.depthSampleView[i], NULL ); rt.depthSampleView[i] = VK_NULL_HANDLE; }
		if ( rt.uboMem[i] ) {
			qvkUnmapMemory( vk.device, rt.uboMem[i] );
			qvkFreeMemory( vk.device, rt.uboMem[i], NULL );
			rt.uboMem[i] = VK_NULL_HANDLE;
		}
		if ( rt.uboBuf[i] ) { qvkDestroyBuffer( vk.device, rt.uboBuf[i], NULL ); rt.uboBuf[i] = VK_NULL_HANDLE; }
		rt.uboMapped[i] = NULL;
	}
	if ( rt.colorSampler ) { qvkDestroySampler( vk.device, rt.colorSampler, NULL ); rt.colorSampler = VK_NULL_HANDLE; }
	if ( rt.depthSampler ) { qvkDestroySampler( vk.device, rt.depthSampler, NULL ); rt.depthSampler = VK_NULL_HANDLE; }
	rt.targetsReady = qfalse;
}

//==========================================================================
//
// Per-frame UBO + the deferred lighting resolve
//
//==========================================================================

static void RT_UpdateUBO( int frame, int readPing, int writePing ) {
	rtUBO_t		*u = (rtUBO_t *)rt.uboMapped[frame];
	float		pv[16], m[16];
	int			i, n;
	qboolean	temporalOn = ( r_rtTemporal && r_rtTemporal->integer && rt.tempPipe != VK_NULL_HANDLE );

	// invViewProj = inverse( Cz * proj * view ), where Cz remaps clip z [-1,1]->[0,1]
	// (matches VK_SetModelMatrix's per-column z fix in vk_backend.c).  m is the full VP.
	RT_Mat4Mul( rt.camProj, rt.camView, pv );
	for ( i = 0; i < 4; i++ ) {
		m[i * 4 + 0] = pv[i * 4 + 0];
		m[i * 4 + 1] = pv[i * 4 + 1];
		m[i * 4 + 2] = 0.5f * ( pv[i * 4 + 2] + pv[i * 4 + 3] );
		m[i * 4 + 3] = pv[i * 4 + 3];
	}
	Com_Memcpy( rt.camVP, m, sizeof( m ) );
	if ( !RT_Mat4Inverse( m, u->invViewProj ) ) {
		memset( u->invViewProj, 0, sizeof( u->invViewProj ) );
	}

	u->eye[0] = rt.camEye[0]; u->eye[1] = rt.camEye[1]; u->eye[2] = rt.camEye[2]; u->eye[3] = 0.0f;
	u->screen[0] = (float)vk.renderExtent.width;
	u->screen[1] = (float)vk.renderExtent.height;
	u->screen[2] = 1.0f / (float)vk.renderExtent.width;
	u->screen[3] = 1.0f / (float)vk.renderExtent.height;

	u->p0[0] = r_rtAmbientScale ? r_rtAmbientScale->value : 0.6f;
	u->p0[1] = r_rtGIIntensity ? r_rtGIIntensity->value : 1.0f;
	u->p0[2] = r_rtGI && r_rtGI->integer ? (float)( r_rtRays ? r_rtRays->integer : 4 ) : 0.0f;
	u->p0[3] = ( r_rtGI && r_rtGI->integer ) ? 1.0f : 0.0f;

	// snapshot the scene's dynamic lights (world-space origins)
	n = backEnd.refdef.num_dlights;
	if ( n > RT_MAX_DLIGHTS ) {
		n = RT_MAX_DLIGHTS;
	}
	for ( i = 0; i < n; i++ ) {
		const dlight_t *d = &backEnd.refdef.dlights[i];
		u->dl[i].posRad[0] = d->origin[0];
		u->dl[i].posRad[1] = d->origin[1];
		u->dl[i].posRad[2] = d->origin[2];
		u->dl[i].posRad[3] = d->radius;
		u->dl[i].color[0] = d->color[0];
		u->dl[i].color[1] = d->color[1];
		u->dl[i].color[2] = d->color[2];
		u->dl[i].color[3] = 0.0f;
	}
	// debug: a bright shadow-casting light above the camera so dynamic shadows can be
	// inspected without firing a weapon (r_rtTestLight = intensity; 0 = off).
	if ( r_rtTestLight && r_rtTestLight->value > 0.0f && n < RT_MAX_DLIGHTS ) {
		float in = r_rtTestLight->value;
		u->dl[n].posRad[0] = rt.camEye[0];
		u->dl[n].posRad[1] = rt.camEye[1];
		u->dl[n].posRad[2] = rt.camEye[2] + 200.0f;
		u->dl[n].posRad[3] = 2000.0f;
		u->dl[n].color[0] = in; u->dl[n].color[1] = in; u->dl[n].color[2] = in; u->dl[n].color[3] = 0.0f;
		n++;
	}
	u->p1[0] = (float)n;
	u->p1[1] = rt.worldBuilt ? 1.0f : 0.0f;
	u->p1[2] = 512.0f;		// indirect gather ray length (world units)
	u->p1[3] = 0.0f;

	// temporal reprojection data: read the history written 2 frames ago for this slot
	Com_Memcpy( u->prevViewProj, rt.tHistVP[readPing][frame], sizeof( u->prevViewProj ) );
	u->prevEye[0] = rt.tHistEye[readPing][frame][0];
	u->prevEye[1] = rt.tHistEye[readPing][frame][1];
	u->prevEye[2] = rt.tHistEye[readPing][frame][2];
	u->prevEye[3] = 0.0f;
	u->temporal[0] = temporalOn ? 1.0f : 0.0f;
	u->temporal[1] = 0.2f;									// blend alpha (new-sample weight): lower = cleaner
															// but more lag/ghosting on moving lights

	u->temporal[2] = ( temporalOn && rt.tHistValid[readPing][frame] ) ? 1.0f : 0.0f;
	u->temporal[3] = temporalOn ? (float)rt.frameCounter : 0.0f;	// 0 keeps the noise static when off

	// record what this frame writes into the history (read back 2 frames from now)
	Com_Memcpy( rt.tHistVP[writePing][frame], rt.camVP, sizeof( rt.camVP ) );
	rt.tHistEye[writePing][frame][0] = rt.camEye[0];
	rt.tHistEye[writePing][frame][1] = rt.camEye[1];
	rt.tHistEye[writePing][frame][2] = rt.camEye[2];
	rt.tHistValid[writePing][frame] = qtrue;
	rt.tHistPing[frame] = writePing;
	rt.frameCounter = ( rt.frameCounter + 1 ) & 0xFFFF;
}

static void RT_WriteDescriptors( int frame ) {
	VkDescriptorImageInfo	scene, depth, out, giOut, albedo;
	VkDescriptorBufferInfo	geoI, idxI, uboI;
	VkWriteDescriptorSetAccelerationStructureKHR	tlasInfo;
	VkWriteDescriptorSet	w[9];

	memset( &scene, 0, sizeof( scene ) );
	scene.sampler = rt.colorSampler;
	scene.imageView = vk.offscreenView[frame];
	scene.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset( &depth, 0, sizeof( depth ) );
	depth.sampler = rt.depthSampler;
	depth.imageView = rt.depthSampleView[frame];
	depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset( &out, 0, sizeof( out ) );
	out.imageView = rt.rtColorView[frame];
	out.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	memset( &giOut, 0, sizeof( giOut ) );
	giOut.imageView = rt.giView[frame];
	giOut.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	memset( &albedo, 0, sizeof( albedo ) );
	albedo.sampler = rt.colorSampler;
	albedo.imageView = vk.albedoView[frame];
	albedo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	geoI.buffer = rt.geoBuf; geoI.offset = 0; geoI.range = VK_WHOLE_SIZE;
	idxI.buffer = rt.idxBuf; idxI.offset = 0; idxI.range = VK_WHOLE_SIZE;
	uboI.buffer = rt.uboBuf[frame]; uboI.offset = 0; uboI.range = sizeof( rtUBO_t );

	memset( &tlasInfo, 0, sizeof( tlasInfo ) );
	tlasInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	tlasInfo.accelerationStructureCount = 1;
	tlasInfo.pAccelerationStructures = &rt.tlas;

	memset( w, 0, sizeof( w ) );
	w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = rt.sets[frame]; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &scene;
	w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = rt.sets[frame]; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &depth;
	w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = rt.sets[frame]; w[2].dstBinding = 2; w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo = &out;
	w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = rt.sets[frame]; w[3].dstBinding = 3; w[3].descriptorCount = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[3].pNext = &tlasInfo;
	w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[4].dstSet = rt.sets[frame]; w[4].dstBinding = 4; w[4].descriptorCount = 1; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &geoI;
	w[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[5].dstSet = rt.sets[frame]; w[5].dstBinding = 5; w[5].descriptorCount = 1; w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[5].pBufferInfo = &idxI;
	w[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[6].dstSet = rt.sets[frame]; w[6].dstBinding = 6; w[6].descriptorCount = 1; w[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[6].pBufferInfo = &uboI;
	w[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[7].dstSet = rt.sets[frame]; w[7].dstBinding = 7; w[7].descriptorCount = 1; w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[7].pImageInfo = &giOut;
	w[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[8].dstSet = rt.sets[frame]; w[8].dstBinding = 8; w[8].descriptorCount = 1; w[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[8].pImageInfo = &albedo;

	qvkUpdateDescriptorSets( vk.device, 9, w, 0, NULL );
}

static void RT_WriteBlurDescriptors( int frame ) {
	VkDescriptorImageInfo	gi, depth, out;
	VkDescriptorBufferInfo	uboI;
	VkWriteDescriptorSet	w[4];

	memset( &gi, 0, sizeof( gi ) );
	gi.sampler = rt.colorSampler;
	gi.imageView = rt.giView[frame];
	gi.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset( &depth, 0, sizeof( depth ) );
	depth.sampler = rt.depthSampler;
	depth.imageView = rt.depthSampleView[frame];
	depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset( &out, 0, sizeof( out ) );
	out.imageView = rt.rtColorView[frame];
	out.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	uboI.buffer = rt.uboBuf[frame]; uboI.offset = 0; uboI.range = sizeof( rtUBO_t );

	memset( w, 0, sizeof( w ) );
	w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = rt.blurSets[frame]; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &gi;
	w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = rt.blurSets[frame]; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &depth;
	w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = rt.blurSets[frame]; w[2].dstBinding = 2; w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo = &out;
	w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = rt.blurSets[frame]; w[3].dstBinding = 3; w[3].descriptorCount = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[3].pBufferInfo = &uboI;

	qvkUpdateDescriptorSets( vk.device, 4, w, 0, NULL );
}

static void RT_WriteTempDescriptors( int frame, int readPing, int writePing ) {
	VkDescriptorImageInfo	gi, depth, histR, histW;
	VkDescriptorBufferInfo	uboI;
	VkWriteDescriptorSet	w[5];

	memset( &gi, 0, sizeof( gi ) );
	gi.imageView = rt.giView[frame];
	gi.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	memset( &depth, 0, sizeof( depth ) );
	depth.sampler = rt.depthSampler;
	depth.imageView = rt.depthSampleView[frame];
	depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset( &histR, 0, sizeof( histR ) );
	histR.sampler = rt.colorSampler;
	histR.imageView = rt.tHistView[readPing][frame];
	histR.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset( &histW, 0, sizeof( histW ) );
	histW.imageView = rt.tHistView[writePing][frame];
	histW.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	uboI.buffer = rt.uboBuf[frame]; uboI.offset = 0; uboI.range = sizeof( rtUBO_t );

	memset( w, 0, sizeof( w ) );
	w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = rt.tempSets[frame]; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &gi;
	w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = rt.tempSets[frame]; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &depth;
	w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = rt.tempSets[frame]; w[2].dstBinding = 2; w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &histR;
	w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = rt.tempSets[frame]; w[3].dstBinding = 3; w[3].descriptorCount = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[3].pImageInfo = &histW;
	w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[4].dstSet = rt.tempSets[frame]; w[4].dstBinding = 4; w[4].descriptorCount = 1; w[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[4].pBufferInfo = &uboI;

	qvkUpdateDescriptorSets( vk.device, 5, w, 0, NULL );
}

/*
================
VK_RT_BlitToSwapchain

Copy the (relit + transparent-composited) offscreen scene colour to the swapchain
image, leaving it COLOR_ATTACHMENT_OPTIMAL.  Used for the final present blit and as
the whole resolve when RT can't run (menu / no world AS -- offscreen holds raster).
================
*/
void VK_RT_BlitToSwapchain( void ) {
	int			frame = vk.frameIndex;
	VkImageBlit	region;

	RT_ImageBarrier( vk.offscreenImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
	RT_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		0, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

	memset( &region, 0, sizeof( region ) );
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.srcSubresource.layerCount = 1;
	region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.dstSubresource.layerCount = 1;
	region.srcOffsets[1].x = (int32_t)vk.renderExtent.width;  region.srcOffsets[1].y = (int32_t)vk.renderExtent.height; region.srcOffsets[1].z = 1;
	region.dstOffsets[1].x = (int32_t)vk.extent.width;        region.dstOffsets[1].y = (int32_t)vk.extent.height;       region.dstOffsets[1].z = 1;
	qvkCmdBlitImage( vk.cmd,
		vk.offscreenImage[frame], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region, VK_FILTER_NEAREST );

	RT_ImageBarrier( vk.swapchainImages[vk.swapchainIndex], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );
}

/*
================
VK_RT_RelightOffscreen

Run the ray-query lighting + denoise compute passes over the opaque offscreen +
G-buffer and blit the relit result BACK into the offscreen (in place), so the
transparent pass that follows blends over the ray-traced image.  Leaves the
offscreen in COLOR_ATTACHMENT_OPTIMAL and depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
No-op (offscreen kept as the raster image) until the world AS + targets are ready.
================
*/
void VK_RT_RelightOffscreen( void ) {
	int			frame = vk.frameIndex;
	int			readPing, writePing;
	uint32_t	gx, gy;
	VkImageBlit	region;

	if ( !rt.targetsReady || !rt.worldBuilt || !rt.camValid ) {
		return;		// leave the offscreen as the rasterised image
	}

	// temporal history ping-pong for this slot: read the data written 2 frames ago
	// (race-free via frameFence), write to the other ping for 2 frames from now.
	readPing  = rt.tHistPing[frame];
	writePing = readPing ^ 1;

	RT_UpdateUBO( frame, readPing, writePing );
	RT_WriteDescriptors( frame );
	RT_WriteTempDescriptors( frame, readPing, writePing );
	RT_WriteBlurDescriptors( frame );

	// inputs -> shader-readable; outputs -> general (storage write)
	RT_ImageBarrier( vk.offscreenImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	RT_ImageBarrier( vk.depthImage[frame], VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	RT_ImageBarrier( vk.albedoImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	RT_ImageBarrier( rt.rtColorImg[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		0, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	RT_ImageBarrier( rt.giImg[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		0, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	gx = ( vk.renderExtent.width + 7 ) / 8;
	gy = ( vk.renderExtent.height + 7 ) / 8;

	// pass 1: lighting (writes lit -> rtColor, raw per-frame-jittered indirect -> giImg)
	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.pipe );
	qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.pipeLayout, 0, 1, &rt.sets[frame], 0, NULL );
	qvkCmdDispatch( vk.cmd, gx, gy, 1 );

	// pass 1.5: temporal accumulation -- reproject the 2-frame-old history into giImg so
	// the per-frame noise averages out over time instead of sitting static on screen.
	// giImg: light-write -> temporal read+write (same GENERAL layout, memory hazard only)
	RT_ImageBarrier( rt.giImg[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	RT_ImageBarrier( rt.tHistImg[readPing][frame], VK_IMAGE_ASPECT_COLOR_BIT,
		rt.tHistLayout[readPing][frame], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		0, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	RT_ImageBarrier( rt.tHistImg[writePing][frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,		// we overwrite every pixel
		0, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	rt.tHistLayout[readPing][frame]  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	rt.tHistLayout[writePing][frame] = VK_IMAGE_LAYOUT_GENERAL;

	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.tempPipe );
	qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.tempPipeLayout, 0, 1, &rt.tempSets[frame], 0, NULL );
	qvkCmdDispatch( vk.cmd, gx, gy, 1 );

	// giImg: temporal-write -> sampled (blur); rtColor: light-write -> storage read+write
	RT_ImageBarrier( rt.giImg[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
	RT_ImageBarrier( rt.rtColorImg[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	// pass 2: denoise the indirect and composite over rtColor
	qvkCmdBindPipeline( vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.blurPipe );
	qvkCmdBindDescriptorSets( vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.blurPipeLayout, 0, 1, &rt.blurSets[frame], 0, NULL );
	qvkCmdDispatch( vk.cmd, gx, gy, 1 );

	// blit the relit rtColor BACK into the offscreen (in place), so the transparent
	// pass blends over the ray-traced image.  rtColor -> transfer src ; offscreen
	// (was shader-read for the compute) -> transfer dst ; blit ; restore layouts.
	RT_ImageBarrier( rt.rtColorImg[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
	RT_ImageBarrier( vk.offscreenImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

	memset( &region, 0, sizeof( region ) );
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.srcSubresource.layerCount = 1;
	region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.dstSubresource.layerCount = 1;
	region.srcOffsets[1].x = (int32_t)vk.renderExtent.width;  region.srcOffsets[1].y = (int32_t)vk.renderExtent.height; region.srcOffsets[1].z = 1;
	region.dstOffsets[1].x = (int32_t)vk.renderExtent.width;  region.dstOffsets[1].y = (int32_t)vk.renderExtent.height; region.dstOffsets[1].z = 1;
	qvkCmdBlitImage( vk.cmd,
		rt.rtColorImg[frame], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		vk.offscreenImage[frame], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region, VK_FILTER_NEAREST );

	RT_ImageBarrier( vk.offscreenImage[frame], VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );

	// depth back to attachment layout for the transparent pass (depth-tested) that follows
	RT_ImageBarrier( vk.depthImage[frame], VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT );
}
