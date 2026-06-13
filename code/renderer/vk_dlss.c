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
// vk_dlss.c -- NVIDIA DLSS integration for the Vulkan backend.
//
// All NVIDIA NGX SDK usage is isolated here behind USE_DLSS.  The SDK is
// proprietary and cannot be redistributed in this tree, so by default USE_DLSS
// is NOT defined: the module still builds and r_dlss still lowers the render
// resolution (the scene is rasterised small and linear-upscaled to the display),
// which is a genuine, visible speed-up.  When a developer vendors the NGX SDK
// (renderer/nvsdk_ngx/, see DLSS_VULKAN_REFERENCE.md) and defines USE_DLSS, the
// real temporal super-resolution network kicks in.
//
// DLSS is x64 + Vulkan only -- which is exactly why this engine was migrated
// from Win32 to x64.
//
#include "vk_local.h"
#include "vk_dlss.h"

// Optionally auto-enable the real path when the vendored SDK header is present.
#if !defined(USE_DLSS) && defined(__has_include)
#  if __has_include("nvsdk_ngx/include/nvsdk_ngx.h")
#    define USE_DLSS 1
#  endif
#endif

#ifdef USE_DLSS
#  include "nvsdk_ngx/include/nvsdk_ngx.h"
#  include "nvsdk_ngx/include/nvsdk_ngx_vk.h"
#  include "nvsdk_ngx/include/nvsdk_ngx_helpers.h"
#  include "nvsdk_ngx/include/nvsdk_ngx_helpers_vk.h"
// A project GUID identifies the integration to the driver.  Generate your own.
#  define DLSS_PROJECT_ID	"a0f57b54-1daf-4934-90ae-c4035c19df04"
#endif

// ----------------------------------------------------------------------------
// Per-mode upscale ratio (display / render) along each axis.  These match the
// published DLSS presets and are the authoritative fallback when the NGX
// "optimal settings" query is unavailable.
// ----------------------------------------------------------------------------
static float DLSS_ModeRatio( vkDlssMode_t mode ) {
	switch ( mode ) {
		case VK_DLSS_DLAA:			return 1.0f;	// native res -- pure AA, no upscale
		case VK_DLSS_QUALITY:		return 1.5f;
		case VK_DLSS_BALANCED:		return 1.724f;
		case VK_DLSS_PERFORMANCE:	return 2.0f;
		case VK_DLSS_ULTRA_PERF:	return 3.0f;
		default:					return 1.0f;	// Off
	}
}

const char *VK_DLSS_ModeName( int mode ) {
	switch ( mode ) {
		case VK_DLSS_QUALITY:		return "Quality";
		case VK_DLSS_BALANCED:		return "Balanced";
		case VK_DLSS_PERFORMANCE:	return "Performance";
		case VK_DLSS_ULTRA_PERF:	return "Ultra Performance";
		case VK_DLSS_DLAA:			return "DLAA";
		default:					return "Off";
	}
}

// ----------------------------------------------------------------------------
// Halton low-discrepancy sequence -- the recommended DLSS jitter pattern.
// ----------------------------------------------------------------------------
static float Halton( int index, int base ) {
	float f = 1.0f, r = 0.0f;
	while ( index > 0 ) {
		f /= (float)base;
		r += f * (float)( index % base );
		index /= base;
	}
	return r;
}

void VK_DLSS_Jitter( int frame, uint32_t renderW, uint32_t displayW, float *jx, float *jy ) {
	int phases;
	int idx;

	// 8 * (display/render)^2 phases, clamped to a sane range (guide recommendation).
	if ( renderW == 0 ) {
		*jx = *jy = 0.0f;
		return;
	}
	{
		float ratio = (float)displayW / (float)renderW;
		phases = (int)( 8.0f * ratio * ratio + 0.5f );
		if ( phases < 8 )  phases = 8;
		if ( phases > 128 ) phases = 128;
	}
	idx = ( frame % phases ) + 1;	// Halton is 1-based

	// Halton in [0,1) -> centred sub-pixel offset in [-0.5,+0.5].
	*jx = Halton( idx, 2 ) - 0.5f;
	*jy = Halton( idx, 3 ) - 0.5f;
}

// ----------------------------------------------------------------------------
// Render-resolution selection (always available, SDK or not).
// ----------------------------------------------------------------------------
qboolean VK_DLSS_RenderResolution( vkDlssMode_t mode, uint32_t outW, uint32_t outH,
								   uint32_t *renderW, uint32_t *renderH ) {
	float ratio;

	if ( mode <= VK_DLSS_OFF || mode > VK_DLSS_ULTRA_PERF ) {
		return qfalse;
	}

#ifdef USE_DLSS
	// Prefer the driver's optimal render resolution when NGX is live.
	if ( VK_DLSS_Available() ) {
		extern qboolean VK_DLSS_OptimalSettings( vkDlssMode_t, uint32_t, uint32_t, uint32_t *, uint32_t * );
		if ( VK_DLSS_OptimalSettings( mode, outW, outH, renderW, renderH ) ) {
			return qtrue;
		}
	}
#endif

	ratio = DLSS_ModeRatio( mode );
	*renderW = (uint32_t)( (float)outW / ratio + 0.5f );
	*renderH = (uint32_t)( (float)outH / ratio + 0.5f );
	if ( *renderW < 32 )  *renderW = 32;
	if ( *renderH < 32 )  *renderH = 32;
	return qtrue;
}

// ============================================================================
//  Real NGX path (USE_DLSS) vs. portable stubs.
// ============================================================================
#ifdef USE_DLSS

static qboolean				dlss_ngxInited;
static qboolean				dlss_available;
static NVSDK_NGX_Parameter	*dlss_params;		// capability parameters (NGX-owned)
static NVSDK_NGX_Handle		*dlss_feature;		// the live SuperSampling feature

qboolean VK_DLSS_Available( void ) {
	return dlss_available;
}

void VK_DLSS_GetRequiredInstanceExtensions( const char **names, int *count, int maxCount ) {
	unsigned int		instCnt = 0, devCnt = 0;
	const char			**instExt = NULL, **devExt = NULL;
	unsigned int		i;

	if ( NVSDK_NGX_FAILED( NVSDK_NGX_VULKAN_RequiredExtensions( &instCnt, &instExt, &devCnt, &devExt ) ) ) {
		return;
	}
	for ( i = 0; i < instCnt && *count < maxCount; i++ ) {
		names[ (*count)++ ] = instExt[i];
	}
}

void VK_DLSS_GetRequiredDeviceExtensions( VkPhysicalDevice phys, const char **names, int *count, int maxCount ) {
	unsigned int		instCnt = 0, devCnt = 0;
	const char			**instExt = NULL, **devExt = NULL;
	unsigned int		i;

	(void)phys;
	if ( NVSDK_NGX_FAILED( NVSDK_NGX_VULKAN_RequiredExtensions( &instCnt, &instExt, &devCnt, &devExt ) ) ) {
		return;
	}
	for ( i = 0; i < devCnt && *count < maxCount; i++ ) {
		names[ (*count)++ ] = devExt[i];
	}
}

void VK_DLSS_Init( void ) {
	NVSDK_NGX_Result	r;
	int					avail = 0;

	dlss_available = qfalse;
	if ( dlss_ngxInited ) {
		return;
	}

	r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
			DLSS_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
			L".", vk.instance, vk.physicalDevice, vk.device,
			NULL, NULL, NULL, NVSDK_NGX_Version_API );
	if ( NVSDK_NGX_FAILED( r ) ) {
		ri.Printf( PRINT_ALL, "DLSS: NGX init failed (0x%08x)\n", (unsigned)r );
		return;
	}
	dlss_ngxInited = qtrue;

	if ( NVSDK_NGX_FAILED( NVSDK_NGX_VULKAN_GetCapabilityParameters( &dlss_params ) ) ) {
		ri.Printf( PRINT_ALL, "DLSS: could not get capability parameters\n" );
		return;
	}

	NVSDK_NGX_Parameter_GetI( dlss_params, NVSDK_NGX_Parameter_SuperSampling_Available, &avail );
	if ( !avail ) {
		ri.Printf( PRINT_ALL, "DLSS: not supported on this GPU/driver\n" );
		return;
	}

	dlss_available = qtrue;
	ri.Printf( PRINT_ALL, "DLSS: NGX super-resolution available\n" );
}

void VK_DLSS_Shutdown( void ) {
	VK_DLSS_ReleaseFeature();
	if ( dlss_ngxInited ) {
		NVSDK_NGX_VULKAN_Shutdown1( vk.device );
		dlss_ngxInited = qfalse;
	}
	dlss_available = qfalse;
	dlss_params = NULL;
}

static NVSDK_NGX_PerfQuality_Value DLSS_PerfValue( vkDlssMode_t mode ) {
	switch ( mode ) {
		case VK_DLSS_QUALITY:		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
		case VK_DLSS_BALANCED:		return NVSDK_NGX_PerfQuality_Value_Balanced;
		case VK_DLSS_PERFORMANCE:	return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		case VK_DLSS_ULTRA_PERF:	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		case VK_DLSS_DLAA:			return NVSDK_NGX_PerfQuality_Value_DLAA;
		default:					return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

qboolean VK_DLSS_OptimalSettings( vkDlssMode_t mode, uint32_t outW, uint32_t outH,
								  uint32_t *renderW, uint32_t *renderH ) {
	unsigned int	rw = 0, rh = 0, maxW, maxH, minW, minH;
	float			sharp;

	if ( !dlss_available || !dlss_params ) {
		return qfalse;
	}
	if ( NVSDK_NGX_FAILED( NGX_DLSS_GET_OPTIMAL_SETTINGS(
			dlss_params, outW, outH, DLSS_PerfValue( mode ),
			&rw, &rh, &maxW, &maxH, &minW, &minH, &sharp ) ) || rw == 0 || rh == 0 ) {
		return qfalse;
	}
	*renderW = rw;
	*renderH = rh;
	return qtrue;
}

qboolean VK_DLSS_CreateFeature( VkCommandBuffer cmd, vkDlssMode_t mode,
								uint32_t renderW, uint32_t renderH,
								uint32_t displayW, uint32_t displayH ) {
	NVSDK_NGX_DLSS_Create_Params	cp;
	NVSDK_NGX_Result				r;

	if ( !dlss_available ) {
		return qfalse;
	}
	VK_DLSS_ReleaseFeature();

	memset( &cp, 0, sizeof( cp ) );
	cp.Feature.InWidth			= renderW;
	cp.Feature.InHeight			= renderH;
	cp.Feature.InTargetWidth	= displayW;
	cp.Feature.InTargetHeight	= displayH;
	cp.Feature.InPerfQualityValue = DLSS_PerfValue( mode );
	// Render-res motion vectors; LDR colour (no IsHDR); engine uses a standard
	// 0=near depth, so DepthInverted is left off.  We have no exposure texture,
	// so let DLSS auto-expose.
	cp.InFeatureCreateFlags		= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
								| NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	cp.InEnableOutputSubrects	= 0;

	r = NGX_VULKAN_CREATE_DLSS_EXT( cmd, 1, 1, &dlss_feature, dlss_params, &cp );
	if ( NVSDK_NGX_FAILED( r ) ) {
		ri.Printf( PRINT_ALL, "DLSS: feature creation failed (0x%08x)\n", (unsigned)r );
		dlss_feature = NULL;
		return qfalse;
	}
	ri.Printf( PRINT_ALL, "DLSS: feature created %ux%u -> %ux%u (%s)\n",
		renderW, renderH, displayW, displayH, VK_DLSS_ModeName( mode ) );
	return qtrue;
}

void VK_DLSS_ReleaseFeature( void ) {
	if ( dlss_feature ) {
		NVSDK_NGX_VULKAN_ReleaseFeature( dlss_feature );
		dlss_feature = NULL;
	}
}

static NVSDK_NGX_Resource_VK DLSS_WrapImage( VkImage img, VkImageView view, VkFormat fmt,
											 uint32_t w, uint32_t h, VkImageAspectFlags aspect,
											 bool readWrite ) {
	VkImageSubresourceRange		sub;
	sub.aspectMask		= aspect;
	sub.baseMipLevel	= 0;
	sub.levelCount		= 1;
	sub.baseArrayLayer	= 0;
	sub.layerCount		= 1;
	return NVSDK_NGX_Create_ImageView_Resource_VK( view, img, sub, fmt, w, h, readWrite );
}

qboolean VK_DLSS_Evaluate( VkCommandBuffer cmd,
						VkImage colorImg, VkImageView colorView,
						VkImage depthImg, VkImageView depthView,
						VkImage mvImg,    VkImageView mvView,
						VkImage outImg,   VkImageView outView,
						uint32_t renderW, uint32_t renderH,
						uint32_t displayW, uint32_t displayH,
						float jitterX, float jitterY, qboolean reset ) {
	NVSDK_NGX_Resource_VK			colorR, depthR, mvR, outR;
	NVSDK_NGX_VK_DLSS_Eval_Params	ep;
	NVSDK_NGX_Result				r;

	if ( !dlss_available || !dlss_feature ) {
		return qfalse;
	}

	colorR = DLSS_WrapImage( colorImg, colorView, vk.surfaceFormat.format, renderW, renderH,
							 VK_IMAGE_ASPECT_COLOR_BIT, false );
	depthR = DLSS_WrapImage( depthImg, depthView, vk.depthFormat, renderW, renderH,
							 VK_IMAGE_ASPECT_DEPTH_BIT, false );
	mvR    = DLSS_WrapImage( mvImg, mvView, VK_FORMAT_R16G16_SFLOAT, renderW, renderH,
							 VK_IMAGE_ASPECT_COLOR_BIT, false );
	outR   = DLSS_WrapImage( outImg, outView, vk.surfaceFormat.format, displayW, displayH,
							 VK_IMAGE_ASPECT_COLOR_BIT, true );

	memset( &ep, 0, sizeof( ep ) );
	ep.Feature.pInColor		= &colorR;
	ep.Feature.pInOutput	= &outR;
	ep.pInDepth				= &depthR;
	ep.pInMotionVectors		= &mvR;
	ep.InJitterOffsetX		= jitterX;
	ep.InJitterOffsetY		= jitterY;
	ep.InRenderSubrectDimensions.Width  = renderW;
	ep.InRenderSubrectDimensions.Height = renderH;
	ep.InReset				= reset ? 1 : 0;
	ep.InMVScaleX			= 1.0f;		// motion vectors already in render-res pixels
	ep.InMVScaleY			= 1.0f;

	r = NGX_VULKAN_EVALUATE_DLSS_EXT( cmd, dlss_feature, dlss_params, &ep );
	if ( NVSDK_NGX_FAILED( r ) ) {
		ri.Printf( PRINT_DEVELOPER, "DLSS: evaluate failed (0x%08x)\n", (unsigned)r );
		return qfalse;
	}
	return qtrue;
}

#else // !USE_DLSS  -- portable stubs (no NGX dependency)

qboolean VK_DLSS_Available( void ) { return qfalse; }

void VK_DLSS_GetRequiredInstanceExtensions( const char **names, int *count, int maxCount ) {
	(void)names; (void)count; (void)maxCount;
}
void VK_DLSS_GetRequiredDeviceExtensions( VkPhysicalDevice phys, const char **names, int *count, int maxCount ) {
	(void)phys; (void)names; (void)count; (void)maxCount;
}
void VK_DLSS_Init( void )		{ }
void VK_DLSS_Shutdown( void )	{ }
void VK_DLSS_ReleaseFeature( void ) { }

qboolean VK_DLSS_CreateFeature( VkCommandBuffer cmd, vkDlssMode_t mode,
								uint32_t renderW, uint32_t renderH,
								uint32_t displayW, uint32_t displayH ) {
	(void)cmd; (void)mode; (void)renderW; (void)renderH; (void)displayW; (void)displayH;
	return qfalse;	// caller upscales the low-res scene with a plain linear blit
}

qboolean VK_DLSS_Evaluate( VkCommandBuffer cmd,
						VkImage colorImg, VkImageView colorView,
						VkImage depthImg, VkImageView depthView,
						VkImage mvImg,    VkImageView mvView,
						VkImage outImg,   VkImageView outView,
						uint32_t renderW, uint32_t renderH,
						uint32_t displayW, uint32_t displayH,
						float jitterX, float jitterY, qboolean reset ) {
	(void)cmd; (void)colorImg; (void)colorView; (void)depthImg; (void)depthView;
	(void)mvImg; (void)mvView; (void)outImg; (void)outView;
	(void)renderW; (void)renderH; (void)displayW; (void)displayH;
	(void)jitterX; (void)jitterY; (void)reset;
	return qfalse;
}

#endif // USE_DLSS
