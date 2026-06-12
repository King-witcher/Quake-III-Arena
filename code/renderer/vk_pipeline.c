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
	VkDescriptorSetLayout			sets[2];
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

	// push constant: the 4x4 MVP, vertex stage
	memset( &pushRange, 0, sizeof( pushRange ) );
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushRange.offset = 0;
	pushRange.size = 16 * sizeof( float );

	sets[0] = vk.descriptorSetLayout;
	sets[1] = vk.descriptorSetLayout;

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

	// shader modules (the multitexture pipeline reuses single.vert -- it already
	// forwards both texcoord sets -- with a separate module to keep cleanup simple)
	vk.shaderVert[VK_SHADER_SINGLE] = VK_CreateShaderModule( vk_spv_single_vert, sizeof( vk_spv_single_vert ) );
	vk.shaderFrag[VK_SHADER_SINGLE] = VK_CreateShaderModule( vk_spv_single_frag, sizeof( vk_spv_single_frag ) );
	vk.shaderVert[VK_SHADER_MULTI]  = VK_CreateShaderModule( vk_spv_single_vert, sizeof( vk_spv_single_vert ) );
	vk.shaderFrag[VK_SHADER_MULTI]  = VK_CreateShaderModule( vk_spv_multi_frag, sizeof( vk_spv_multi_frag ) );

	memset( &cacheInfo, 0, sizeof( cacheInfo ) );
	cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	VK_CHECK( qvkCreatePipelineCache( vk.device, &cacheInfo, NULL, &vk.pipelineCache ) );

	s_numPipelines = 0;
	s_lastPipeline = NULL;
	return qtrue;
}

void VK_ShutdownPipelines( void ) {
	int i;

	for ( i = 0; i < s_numPipelines; i++ ) {
		qvkDestroyPipeline( vk.device, s_pipelines[i].pipeline, NULL );
	}
	s_numPipelines = 0;
	s_lastPipeline = NULL;

	if ( vk.pipelineCache ) { qvkDestroyPipelineCache( vk.device, vk.pipelineCache, NULL ); vk.pipelineCache = VK_NULL_HANDLE; }
	for ( i = 0; i < VK_SHADER_COUNT; i++ ) {
		if ( vk.shaderVert[i] ) { qvkDestroyShaderModule( vk.device, vk.shaderVert[i], NULL ); vk.shaderVert[i] = VK_NULL_HANDLE; }
		if ( vk.shaderFrag[i] ) { qvkDestroyShaderModule( vk.device, vk.shaderFrag[i], NULL ); vk.shaderFrag[i] = VK_NULL_HANDLE; }
	}
	if ( vk.pipelineLayout[1] ) { qvkDestroyPipelineLayout( vk.device, vk.pipelineLayout[1], NULL ); vk.pipelineLayout[1] = VK_NULL_HANDLE; }
	if ( vk.pipelineLayout[2] ) { qvkDestroyPipelineLayout( vk.device, vk.pipelineLayout[2], NULL ); vk.pipelineLayout[2] = VK_NULL_HANDLE; }
	if ( vk.descriptorSetLayout ) { qvkDestroyDescriptorSetLayout( vk.device, vk.descriptorSetLayout, NULL ); vk.descriptorSetLayout = VK_NULL_HANDLE; }
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

	numSets = ( key->shaderType == VK_SHADER_MULTI ) ? 2 : 1;

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
