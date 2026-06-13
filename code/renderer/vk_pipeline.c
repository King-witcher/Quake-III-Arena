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
// vk_pipeline.c -- descriptor/pipeline layouts and the GLS_* state -> VkPipeline
// translation, cached so each distinct state combination is built only once.
//
#include "vk_local.h"
#include "vk_spv.h"

#define VK_MAX_PIPELINES	1024

typedef struct {
	vkPipelineKey_t	key;
	VkPipeline		pipeline;
} vkPipelineEntry_t;

static vkPipelineEntry_t	s_pipelines[VK_MAX_PIPELINES];
static int					s_numPipelines;
static vkPipelineEntry_t	*s_lastPipeline;	// short-circuit for consecutive identical draws

/*
================
VK_SrcBlendFactor / VK_DstBlendFactor
================
*/
static VkBlendFactor VK_SrcBlendFactor( unsigned bits ) {
	switch ( bits & GLS_SRCBLEND_BITS ) {
	case GLS_SRCBLEND_ZERO:					return VK_BLEND_FACTOR_ZERO;
	case GLS_SRCBLEND_ONE:					return VK_BLEND_FACTOR_ONE;
	case GLS_SRCBLEND_DST_COLOR:			return VK_BLEND_FACTOR_DST_COLOR;
	case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case GLS_SRCBLEND_SRC_ALPHA:			return VK_BLEND_FACTOR_SRC_ALPHA;
	case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case GLS_SRCBLEND_DST_ALPHA:			return VK_BLEND_FACTOR_DST_ALPHA;
	case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case GLS_SRCBLEND_ALPHA_SATURATE:		return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default:								return VK_BLEND_FACTOR_ONE;
	}
}

static VkBlendFactor VK_DstBlendFactor( unsigned bits ) {
	switch ( bits & GLS_DSTBLEND_BITS ) {
	case GLS_DSTBLEND_ZERO:					return VK_BLEND_FACTOR_ZERO;
	case GLS_DSTBLEND_ONE:					return VK_BLEND_FACTOR_ONE;
	case GLS_DSTBLEND_SRC_COLOR:			return VK_BLEND_FACTOR_SRC_COLOR;
	case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case GLS_DSTBLEND_SRC_ALPHA:			return VK_BLEND_FACTOR_SRC_ALPHA;
	case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case GLS_DSTBLEND_DST_ALPHA:			return VK_BLEND_FACTOR_DST_ALPHA;
	case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	default:								return VK_BLEND_FACTOR_ZERO;
	}
}

static VkShaderModule VK_CreateShaderModule( const uint32_t *code, size_t size ) {
	VkShaderModuleCreateInfo	info;
	VkShaderModule				module;

	memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = size;
	info.pCode = code;
	VK_CHECK( qvkCreateShaderModule( vk.device, &info, NULL, &module ) );
	return module;
}

/*
================
VK_InitLightUbo

Create the Blinn-Phong light uniform buffer's descriptor set layout, pool and
per-frame sets (each permanently pointed at its frame's lightUbo buffer, which
VK_CreateStreamingBuffers allocated just before us).  Used only by VK_SHADER_LIT.
================
*/
static void VK_InitLightUbo( void ) {
	VkDescriptorSetLayoutBinding	binding;
	VkDescriptorSetLayoutCreateInfo	dslInfo;
	VkDescriptorPoolSize			poolSize;
	VkDescriptorPoolCreateInfo		poolInfo;
	VkDescriptorSetAllocateInfo		allocInfo;
	VkDescriptorSetLayout			layouts[VK_NUM_FRAMES];
	int								i;

	// set layout: binding 0 = a single DYNAMIC uniform buffer (the offset selects the
	// per-view ring slot), fragment stage
	memset( &binding, 0, sizeof( binding ) );
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	memset( &dslInfo, 0, sizeof( dslInfo ) );
	dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslInfo.bindingCount = 1;
	dslInfo.pBindings = &binding;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &dslInfo, NULL, &vk.lightUboLayout ) );

	poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	poolSize.descriptorCount = VK_NUM_FRAMES;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = VK_NUM_FRAMES;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vk.lightUboPool ) );

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		layouts[i] = vk.lightUboLayout;
	}
	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = vk.lightUboPool;
	allocInfo.descriptorSetCount = VK_NUM_FRAMES;
	allocInfo.pSetLayouts = layouts;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &allocInfo, vk.lightUboSet ) );

	// the buffers persist for the lifetime of the device, so bind them once
	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		VkDescriptorBufferInfo	bufInfo;
		VkWriteDescriptorSet	write;

		bufInfo.buffer = vk.lightUbo[i];
		bufInfo.offset = 0;					// base; the per-view slot is a dynamic offset at bind time
		bufInfo.range = sizeof( vkLightUbo_t );

		memset( &write, 0, sizeof( write ) );
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = vk.lightUboSet[i];
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		write.pBufferInfo = &bufInfo;
		qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
	}
}

static void VK_ShutdownLightUbo( void ) {
	if ( vk.lightUboPool ) {
		qvkDestroyDescriptorPool( vk.device, vk.lightUboPool, NULL );	// frees the sets
		vk.lightUboPool = VK_NULL_HANDLE;
	}
	if ( vk.lightUboLayout ) {
		qvkDestroyDescriptorSetLayout( vk.device, vk.lightUboLayout, NULL );
		vk.lightUboLayout = VK_NULL_HANDLE;
	}
}

/*
================
VK_InitPipelines

Build the descriptor-set layout, pipeline layouts, shader modules and pipeline
cache.  Must run before any image is created (descriptor sets reference the
layout) and before any pipeline is requested.
================
*/
qboolean VK_InitPipelines( void ) {
	VkDescriptorSetLayoutBinding	binding;
	VkDescriptorSetLayoutCreateInfo	dslInfo;
	VkPushConstantRange				pushRange;
	VkPipelineLayoutCreateInfo		plInfo;
	VkDescriptorSetLayout			sets[3];
	VkPipelineCacheCreateInfo		cacheInfo;

	// set N: a single combined image sampler, fragment stage
	memset( &binding, 0, sizeof( binding ) );
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	memset( &dslInfo, 0, sizeof( dslInfo ) );
	dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslInfo.bindingCount = 1;
	dslInfo.pBindings = &binding;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &dslInfo, NULL, &vk.descriptorSetLayout ) );

	// the Blinn-Phong light UBO set layout (set 2 of the lit pipeline)
	VK_InitLightUbo();

	// push constant: 4x4 MVP (64) + world-space clip plane vec4 (16), vertex stage
	memset( &pushRange, 0, sizeof( pushRange ) );
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushRange.offset = 0;
	pushRange.size = ( 16 + 4 ) * sizeof( float );

	sets[0] = vk.descriptorSetLayout;
	sets[1] = vk.descriptorSetLayout;
	sets[2] = vk.lightUboLayout;

	// layout with a single texture set (single-texture shader)
	memset( &plInfo, 0, sizeof( plInfo ) );
	plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = sets;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pushRange;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &plInfo, NULL, &vk.pipelineLayout[1] ) );

	// layout with two texture sets (multitexture shader, Phase 5)
	plInfo.setLayoutCount = 2;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &plInfo, NULL, &vk.pipelineLayout[2] ) );

	// layout with two texture sets + the light UBO (Blinn-Phong lit shader); same
	// vertex-only push range as the others (specular is from the UBO's dynamic lights)
	plInfo.setLayoutCount = 3;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &plInfo, NULL, &vk.pipelineLayout[3] ) );

	// shader modules (the multitexture pipeline reuses single.vert -- it already
	// forwards both texcoord sets -- with a separate module to keep cleanup simple)
	vk.shaderVert[VK_SHADER_SINGLE] = VK_CreateShaderModule( vk_spv_single_vert, sizeof( vk_spv_single_vert ) );
	vk.shaderFrag[VK_SHADER_SINGLE] = VK_CreateShaderModule( vk_spv_single_frag, sizeof( vk_spv_single_frag ) );
	vk.shaderVert[VK_SHADER_MULTI]  = VK_CreateShaderModule( vk_spv_single_vert, sizeof( vk_spv_single_vert ) );
	vk.shaderFrag[VK_SHADER_MULTI]  = VK_CreateShaderModule( vk_spv_multi_frag, sizeof( vk_spv_multi_frag ) );
	vk.shaderVert[VK_SHADER_LIT]    = VK_CreateShaderModule( vk_spv_lit_vert, sizeof( vk_spv_lit_vert ) );
	vk.shaderFrag[VK_SHADER_LIT]    = VK_CreateShaderModule( vk_spv_lit_frag, sizeof( vk_spv_lit_frag ) );

	// seed the pipeline cache from disk if we saved one previously (faster warm-up;
	// Vulkan validates the cache header and ignores incompatible/foreign data)
	{
		void	*cacheData = NULL;
		int		cacheLen = ri.FS_ReadFile( "vk_pipeline.cache", &cacheData );

		memset( &cacheInfo, 0, sizeof( cacheInfo ) );
		cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		if ( cacheLen > 0 && cacheData ) {
			cacheInfo.initialDataSize = (size_t)cacheLen;
			cacheInfo.pInitialData = cacheData;
		}
		VK_CHECK( qvkCreatePipelineCache( vk.device, &cacheInfo, NULL, &vk.pipelineCache ) );
		if ( cacheData ) {
			ri.FS_FreeFile( cacheData );
		}
	}

	s_numPipelines = 0;
	s_lastPipeline = NULL;

	// FXAA fullscreen pass (offscreen color already exists from VK_CreateSwapchain)
	VK_InitPostProcess();
	return qtrue;
}

void VK_ShutdownPipelines( void ) {
	int i;

	VK_ShutdownPostProcess();

	for ( i = 0; i < s_numPipelines; i++ ) {
		qvkDestroyPipeline( vk.device, s_pipelines[i].pipeline, NULL );
	}
	s_numPipelines = 0;
	s_lastPipeline = NULL;

	if ( vk.pipelineCache ) {
		// persist the warmed cache for next launch
		size_t size = 0;
		qvkGetPipelineCacheData( vk.device, vk.pipelineCache, &size, NULL );
		if ( size > 0 ) {
			void *data = ri.Hunk_AllocateTempMemory( (int)size );
			if ( qvkGetPipelineCacheData( vk.device, vk.pipelineCache, &size, data ) == VK_SUCCESS ) {
				ri.FS_WriteFile( "vk_pipeline.cache", data, (int)size );
			}
			ri.Hunk_FreeTempMemory( data );
		}
		qvkDestroyPipelineCache( vk.device, vk.pipelineCache, NULL );
		vk.pipelineCache = VK_NULL_HANDLE;
	}
	for ( i = 0; i < VK_SHADER_COUNT; i++ ) {
		if ( vk.shaderVert[i] ) { qvkDestroyShaderModule( vk.device, vk.shaderVert[i], NULL ); vk.shaderVert[i] = VK_NULL_HANDLE; }
		if ( vk.shaderFrag[i] ) { qvkDestroyShaderModule( vk.device, vk.shaderFrag[i], NULL ); vk.shaderFrag[i] = VK_NULL_HANDLE; }
	}
	if ( vk.pipelineLayout[1] ) { qvkDestroyPipelineLayout( vk.device, vk.pipelineLayout[1], NULL ); vk.pipelineLayout[1] = VK_NULL_HANDLE; }
	if ( vk.pipelineLayout[2] ) { qvkDestroyPipelineLayout( vk.device, vk.pipelineLayout[2], NULL ); vk.pipelineLayout[2] = VK_NULL_HANDLE; }
	if ( vk.pipelineLayout[3] ) { qvkDestroyPipelineLayout( vk.device, vk.pipelineLayout[3], NULL ); vk.pipelineLayout[3] = VK_NULL_HANDLE; }
	if ( vk.descriptorSetLayout ) { qvkDestroyDescriptorSetLayout( vk.device, vk.descriptorSetLayout, NULL ); vk.descriptorSetLayout = VK_NULL_HANDLE; }
	VK_ShutdownLightUbo();
}

/*
================
VK_CreatePipeline

Translate a state key into a graphics pipeline using dynamic rendering.
================
*/
static VkPipeline VK_CreatePipeline( const vkPipelineKey_t *key ) {
	VkPipelineShaderStageCreateInfo			stages[2];
	VkSpecializationMapEntry				specEntries[2];
	VkSpecializationInfo					specInfo;
	struct { int alphaTest; int combine; } specData;
	int										alphaTest;
	VkVertexInputBindingDescription			vtxBinding;
	VkVertexInputAttributeDescription		vtxAttribs[4];
	VkPipelineVertexInputStateCreateInfo	vtxInput;
	VkPipelineInputAssemblyStateCreateInfo	inputAsm;
	VkPipelineViewportStateCreateInfo		viewportState;
	VkPipelineRasterizationStateCreateInfo	raster;
	VkPipelineMultisampleStateCreateInfo	multisample;
	VkPipelineDepthStencilStateCreateInfo	depthStencil;
	VkPipelineColorBlendAttachmentState		blendAttach;
	VkPipelineColorBlendStateCreateInfo		blend;
	VkDynamicState							dynStates[3];
	VkPipelineDynamicStateCreateInfo		dynamic;
	VkPipelineRenderingCreateInfo			renderingInfo;
	VkGraphicsPipelineCreateInfo			pipelineInfo;
	VkPipeline								pipeline;
	int										numSets;

	// alpha-test spec constant
	if ( key->stateBits & GLS_ATEST_GT_0 )      alphaTest = 1;
	else if ( key->stateBits & GLS_ATEST_LT_80 ) alphaTest = 2;
	else if ( key->stateBits & GLS_ATEST_GE_80 ) alphaTest = 3;
	else                                          alphaTest = 0;

	specData.alphaTest = alphaTest;
	specData.combine = key->multitexEnv;	// 0 MODULATE, 1 ADD, 2 REPLACE (single.frag ignores it)
	specEntries[0].constantID = 0;
	specEntries[0].offset = 0;
	specEntries[0].size = sizeof( int );
	specEntries[1].constantID = 1;
	specEntries[1].offset = sizeof( int );
	specEntries[1].size = sizeof( int );
	specInfo.mapEntryCount = 2;
	specInfo.pMapEntries = specEntries;
	specInfo.dataSize = sizeof( specData );
	specInfo.pData = &specData;

	memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vk.shaderVert[key->shaderType];
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = vk.shaderFrag[key->shaderType];
	stages[1].pName = "main";
	stages[1].pSpecializationInfo = &specInfo;

	// vertex input: vkVertex_t (pos, color, tc0, tc1)
	vtxBinding.binding = 0;
	vtxBinding.stride = sizeof( vkVertex_t );
	vtxBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	vtxAttribs[0].location = 0; vtxAttribs[0].binding = 0; vtxAttribs[0].format = VK_FORMAT_R32G32B32_SFLOAT; vtxAttribs[0].offset = 0;
	vtxAttribs[1].location = 1; vtxAttribs[1].binding = 0; vtxAttribs[1].format = VK_FORMAT_R8G8B8A8_UNORM;   vtxAttribs[1].offset = 12;
	vtxAttribs[2].location = 2; vtxAttribs[2].binding = 0; vtxAttribs[2].format = VK_FORMAT_R32G32_SFLOAT;    vtxAttribs[2].offset = 16;
	vtxAttribs[3].location = 3; vtxAttribs[3].binding = 0; vtxAttribs[3].format = VK_FORMAT_R32G32_SFLOAT;    vtxAttribs[3].offset = 24;

	memset( &vtxInput, 0, sizeof( vtxInput ) );
	vtxInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vtxInput.vertexBindingDescriptionCount = 1;
	vtxInput.pVertexBindingDescriptions = &vtxBinding;
	vtxInput.vertexAttributeDescriptionCount = 4;
	vtxInput.pVertexAttributeDescriptions = vtxAttribs;

	memset( &inputAsm, 0, sizeof( inputAsm ) );
	inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	memset( &viewportState, 0, sizeof( viewportState ) );
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;	// dynamic
	viewportState.scissorCount = 1;		// dynamic

	memset( &raster, 0, sizeof( raster ) );
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = ( key->stateBits & GLS_POLYMODE_LINE ) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	switch ( key->cullType ) {
	case CT_FRONT_SIDED:	raster.cullMode = key->mirror ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT; break;
	case CT_BACK_SIDED:		raster.cullMode = key->mirror ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT; break;
	default:				raster.cullMode = VK_CULL_MODE_NONE; break;
	}
	// We render with negative-height viewports (GL-compatible Y), so GL's CCW
	// winding is preserved for both 2D and 3D -- a single front face works.
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	if ( key->polygonOffset ) {
		raster.depthBiasEnable = VK_TRUE;	// values supplied dynamically (vkCmdSetDepthBias)
	}

	memset( &multisample, 0, sizeof( multisample ) );
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	memset( &depthStencil, 0, sizeof( depthStencil ) );
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = ( key->stateBits & GLS_DEPTHTEST_DISABLE ) ? VK_FALSE : VK_TRUE;
	depthStencil.depthWriteEnable = ( key->stateBits & GLS_DEPTHMASK_TRUE ) ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = ( key->stateBits & GLS_DEPTHFUNC_EQUAL ) ?
		VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;

	memset( &blendAttach, 0, sizeof( blendAttach ) );
	blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
								 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if ( ( key->stateBits & GLS_SRCBLEND_BITS ) || ( key->stateBits & GLS_DSTBLEND_BITS ) ) {
		blendAttach.blendEnable = VK_TRUE;
		blendAttach.srcColorBlendFactor = blendAttach.srcAlphaBlendFactor = VK_SrcBlendFactor( key->stateBits );
		blendAttach.dstColorBlendFactor = blendAttach.dstAlphaBlendFactor = VK_DstBlendFactor( key->stateBits );
		blendAttach.colorBlendOp = blendAttach.alphaBlendOp = VK_BLEND_OP_ADD;
	}

	memset( &blend, 0, sizeof( blend ) );
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttach;

	dynStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynStates[1] = VK_DYNAMIC_STATE_SCISSOR;
	dynStates[2] = VK_DYNAMIC_STATE_DEPTH_BIAS;
	memset( &dynamic, 0, sizeof( dynamic ) );
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 3;
	dynamic.pDynamicStates = dynStates;

	// dynamic rendering: declare the attachment formats this pipeline targets
	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachmentFormats = &vk.surfaceFormat.format;
	renderingInfo.depthAttachmentFormat = vk.depthFormat;
	renderingInfo.stencilAttachmentFormat = vk.depthFormat;

	numSets = ( key->shaderType == VK_SHADER_MULTI ) ? 2 : ( key->shaderType == VK_SHADER_LIT ) ? 3 : 1;

	memset( &pipelineInfo, 0, sizeof( pipelineInfo ) );
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.pNext = &renderingInfo;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vtxInput;
	pipelineInfo.pInputAssemblyState = &inputAsm;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &raster;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &blend;
	pipelineInfo.pDynamicState = &dynamic;
	pipelineInfo.layout = vk.pipelineLayout[numSets];

	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &pipelineInfo, NULL, &pipeline ) );
	return pipeline;
}

/*
================
VK_GetPipeline

Look up (or lazily create + cache) the pipeline for a state key.
================
*/
VkPipeline VK_GetPipeline( const vkPipelineKey_t *key ) {
	int i;

	if ( s_lastPipeline && memcmp( &s_lastPipeline->key, key, sizeof( *key ) ) == 0 ) {
		return s_lastPipeline->pipeline;
	}

	for ( i = 0; i < s_numPipelines; i++ ) {
		if ( memcmp( &s_pipelines[i].key, key, sizeof( *key ) ) == 0 ) {
			s_lastPipeline = &s_pipelines[i];
			return s_pipelines[i].pipeline;
		}
	}

	if ( s_numPipelines >= VK_MAX_PIPELINES ) {
		ri.Error( ERR_DROP, "VK_GetPipeline: VK_MAX_PIPELINES hit" );
	}

	s_pipelines[s_numPipelines].key = *key;
	s_pipelines[s_numPipelines].pipeline = VK_CreatePipeline( key );
	s_lastPipeline = &s_pipelines[s_numPipelines];
	s_numPipelines++;
	return s_lastPipeline->pipeline;
}

//==========================================================================
//
// post-processing (FXAA) -- a fullscreen pass that samples the offscreen scene
// color into the swapchain.  SSAA does not use this (it resolves with a blit).
//
//==========================================================================

/*
================
VK_CreatePostPipeline

Fullscreen-triangle pipeline (no vertex input, depth off, single sample, no depth
attachment) targeting the swapchain format.  vert = fullscreen.vert.
================
*/
static VkPipeline VK_CreatePostPipeline( const uint32_t *frag, size_t fragSize,
										 VkFormat colorFormat, qboolean additive ) {
	VkShaderModule							vert, fragMod;
	VkPipelineShaderStageCreateInfo			stages[2];
	VkPipelineVertexInputStateCreateInfo	vtxInput;
	VkPipelineInputAssemblyStateCreateInfo	inputAsm;
	VkPipelineViewportStateCreateInfo		viewportState;
	VkPipelineRasterizationStateCreateInfo	raster;
	VkPipelineMultisampleStateCreateInfo	multisample;
	VkPipelineDepthStencilStateCreateInfo	depthStencil;
	VkPipelineColorBlendAttachmentState		blendAttach;
	VkPipelineColorBlendStateCreateInfo		blend;
	VkDynamicState							dynStates[2];
	VkPipelineDynamicStateCreateInfo		dynamic;
	VkPipelineRenderingCreateInfo			renderingInfo;
	VkGraphicsPipelineCreateInfo			pipelineInfo;
	VkPipeline								pipeline;

	vert = VK_CreateShaderModule( vk_spv_fullscreen_vert, sizeof( vk_spv_fullscreen_vert ) );
	fragMod = VK_CreateShaderModule( frag, fragSize );

	memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragMod;
	stages[1].pName = "main";

	memset( &vtxInput, 0, sizeof( vtxInput ) );	// no vertex buffer (gl_VertexIndex)
	vtxInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	memset( &inputAsm, 0, sizeof( inputAsm ) );
	inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	memset( &viewportState, 0, sizeof( viewportState ) );
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;	// dynamic
	viewportState.scissorCount = 1;		// dynamic

	memset( &raster, 0, sizeof( raster ) );
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	memset( &multisample, 0, sizeof( multisample ) );
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	memset( &depthStencil, 0, sizeof( depthStencil ) );	// no depth attachment
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

	memset( &blendAttach, 0, sizeof( blendAttach ) );
	if ( additive ) {
		// frame-multisampling accumulate: dst += src (RGB only -- alpha must not grow)
		blendAttach.blendEnable = VK_TRUE;
		blendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttach.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttach.alphaBlendOp = VK_BLEND_OP_ADD;
		blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
									 VK_COLOR_COMPONENT_B_BIT;
	} else {	// opaque write
		blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
									 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}

	memset( &blend, 0, sizeof( blend ) );
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttach;

	dynStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynStates[1] = VK_DYNAMIC_STATE_SCISSOR;
	memset( &dynamic, 0, sizeof( dynamic ) );
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynStates;

	memset( &renderingInfo, 0, sizeof( renderingInfo ) );
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachmentFormats = &colorFormat;
	renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
	renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	memset( &pipelineInfo, 0, sizeof( pipelineInfo ) );
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.pNext = &renderingInfo;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vtxInput;
	pipelineInfo.pInputAssemblyState = &inputAsm;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &raster;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &blend;
	pipelineInfo.pDynamicState = &dynamic;
	pipelineInfo.layout = vk.postLayout;

	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &pipelineInfo, NULL, &pipeline ) );

	qvkDestroyShaderModule( vk.device, vert, NULL );
	qvkDestroyShaderModule( vk.device, fragMod, NULL );
	return pipeline;
}

/*
================
VK_InitPostProcess

Build the FXAA sampler, descriptor pool/sets, pipeline layout and pipeline.  Only
FXAA needs the shader pass; SSAA resolves with a blit and Off renders direct.
================
*/
void VK_InitPostProcess( void ) {
	VkSamplerCreateInfo			sampInfo;
	VkDescriptorPoolSize		poolSize;
	VkDescriptorPoolCreateInfo	poolInfo;
	VkDescriptorSetAllocateInfo	allocInfo;
	VkDescriptorSetLayout		layouts[VK_NUM_FRAMES];
	VkPushConstantRange			pushRange;
	VkPipelineLayoutCreateInfo	plInfo;
	int							i;

	if ( !VK_USES_OFFSCREEN() ) {
		return;		// Off renders straight to the swapchain; no post pass
	}

	memset( &sampInfo, 0, sizeof( sampInfo ) );
	sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampInfo.magFilter = VK_FILTER_LINEAR;
	sampInfo.minFilter = VK_FILTER_LINEAR;
	sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK( qvkCreateSampler( vk.device, &sampInfo, NULL, &vk.postSampler ) );

	// VK_NUM_FRAMES offscreen sampler sets, plus one for the frame-multisampling
	// accumulate buffer (sampled by the resolve pass) when that is enabled.
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = VK_NUM_FRAMES + 1;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = VK_NUM_FRAMES + 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vk.postDescPool ) );

	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		layouts[i] = vk.descriptorSetLayout;	// reuse the single combined-image-sampler layout
	}
	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = vk.postDescPool;
	allocInfo.descriptorSetCount = VK_NUM_FRAMES;
	allocInfo.pSetLayouts = layouts;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &allocInfo, vk.offscreenDesc ) );

	// push constant: vec2 invRes (FXAA) or { vec2 invSrcRes; int factor } (downsample);
	// size the range for the larger of the two (12 bytes)
	pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = 3 * sizeof( float );
	memset( &plInfo, 0, sizeof( plInfo ) );
	plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = &vk.descriptorSetLayout;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pushRange;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &plInfo, NULL, &vk.postLayout ) );

	// DLSS reuses the FXAA fullscreen pass to upscale its sub-display offscreen,
	// so it needs pipeFXAA too (VK_EndFrame routes VK_AA_DLSS to VK_ResolveFXAA).
	if ( vk.aaMode == VK_AA_FXAA || vk.aaMode == VK_AA_DLSS ) {
		vk.pipeFXAA = VK_CreatePostPipeline( vk_spv_fxaa_frag, sizeof( vk_spv_fxaa_frag ),
			vk.surfaceFormat.format, qfalse );
	} else if ( vk.aaMode == VK_AA_SSAA ) {
		vk.pipeDownsample = VK_CreatePostPipeline( vk_spv_downsample_frag, sizeof( vk_spv_downsample_frag ),
			vk.surfaceFormat.format, qfalse );
	}

	// frame multisampling: an additive accumulate pass (offscreen -> float16 sum) and a
	// resolve pass (sum x 1/frames -> held image).  Both reuse accum.frag + postLayout.
	if ( vk.msFrames >= 2 ) {
		VkDescriptorSetAllocateInfo	msAlloc;

		memset( &msAlloc, 0, sizeof( msAlloc ) );
		msAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		msAlloc.descriptorPool = vk.postDescPool;
		msAlloc.descriptorSetCount = 1;
		msAlloc.pSetLayouts = &vk.descriptorSetLayout;
		VK_CHECK( qvkAllocateDescriptorSets( vk.device, &msAlloc, &vk.msAccumDesc ) );

		vk.pipeMSAccum   = VK_CreatePostPipeline( vk_spv_accum_frag, sizeof( vk_spv_accum_frag ),
			VK_FORMAT_R16G16B16A16_SFLOAT, qtrue );
		vk.pipeMSResolve = VK_CreatePostPipeline( vk_spv_accum_frag, sizeof( vk_spv_accum_frag ),
			vk.surfaceFormat.format, qfalse );
	}

	VK_UpdateOffscreenDescriptors();
}

/*
================
VK_UpdateOffscreenDescriptors

(Re)point the FXAA sampler sets at the current offscreen views.  Called once at
init and again whenever VK_CreateSwapchain recreates the offscreen images (resize,
vsync toggle) -- the sets persist in postDescPool but their bound view changes.
================
*/
void VK_UpdateOffscreenDescriptors( void ) {
	int i;

	if ( !VK_USES_OFFSCREEN() || !vk.postDescPool ) {
		return;
	}
	for ( i = 0; i < VK_NUM_FRAMES; i++ ) {
		VkDescriptorImageInfo	imgInfo;
		VkWriteDescriptorSet	write;

		if ( !vk.offscreenView[i] ) {
			continue;
		}
		imgInfo.sampler = vk.postSampler;
		imgInfo.imageView = vk.offscreenView[i];
		imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		memset( &write, 0, sizeof( write ) );
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = vk.offscreenDesc[i];
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imgInfo;
		qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
	}

	// point the resolve pass's sampler at the frame-multisampling accumulate buffer
	if ( vk.msFrames >= 2 && vk.msAccumDesc && vk.msAccumView ) {
		VkDescriptorImageInfo	imgInfo;
		VkWriteDescriptorSet	write;

		imgInfo.sampler = vk.postSampler;
		imgInfo.imageView = vk.msAccumView;
		imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		memset( &write, 0, sizeof( write ) );
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = vk.msAccumDesc;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imgInfo;
		qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
	}
}

/*
================
VK_ShutdownPostProcess
================
*/
void VK_ShutdownPostProcess( void ) {
	if ( vk.pipeFXAA ) {
		qvkDestroyPipeline( vk.device, vk.pipeFXAA, NULL );
		vk.pipeFXAA = VK_NULL_HANDLE;
	}
	if ( vk.pipeDownsample ) {
		qvkDestroyPipeline( vk.device, vk.pipeDownsample, NULL );
		vk.pipeDownsample = VK_NULL_HANDLE;
	}
	if ( vk.pipeMSAccum ) {
		qvkDestroyPipeline( vk.device, vk.pipeMSAccum, NULL );
		vk.pipeMSAccum = VK_NULL_HANDLE;
	}
	if ( vk.pipeMSResolve ) {
		qvkDestroyPipeline( vk.device, vk.pipeMSResolve, NULL );
		vk.pipeMSResolve = VK_NULL_HANDLE;
	}
	if ( vk.postLayout ) {
		qvkDestroyPipelineLayout( vk.device, vk.postLayout, NULL );
		vk.postLayout = VK_NULL_HANDLE;
	}
	if ( vk.postDescPool ) {
		qvkDestroyDescriptorPool( vk.device, vk.postDescPool, NULL );	// frees offscreenDesc
		vk.postDescPool = VK_NULL_HANDLE;
	}
	if ( vk.postSampler ) {
		qvkDestroySampler( vk.device, vk.postSampler, NULL );
		vk.postSampler = VK_NULL_HANDLE;
	}
	memset( vk.offscreenDesc, 0, sizeof( vk.offscreenDesc ) );
	vk.msAccumDesc = VK_NULL_HANDLE;	// freed with postDescPool
}
