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
// vk_instance.c -- Vulkan instance, device and capability bring-up.
//
// VK_Init() builds the full device stack (vulkan-1.dll -> instance -> surface ->
// physical device -> logical device -> swapchain -> per-frame objects) and fills
// glConfig from real device data.  Any failure tears everything back down and
// returns qfalse, which makes R_Init() fall back to OpenGL.
//
#include "vk_local.h"

vk_t	vk;

/*
================
VK_ResultString
================
*/
const char *VK_ResultString( VkResult result ) {
	switch ( result ) {
	case VK_SUCCESS:							return "VK_SUCCESS";
	case VK_NOT_READY:							return "VK_NOT_READY";
	case VK_TIMEOUT:							return "VK_TIMEOUT";
	case VK_EVENT_SET:							return "VK_EVENT_SET";
	case VK_EVENT_RESET:						return "VK_EVENT_RESET";
	case VK_INCOMPLETE:							return "VK_INCOMPLETE";
	case VK_ERROR_OUT_OF_HOST_MEMORY:			return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED:		return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST:					return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED:			return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT:			return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT:		return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT:			return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER:			return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS:				return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED:			return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_SURFACE_LOST_KHR:				return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:		return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
	case VK_SUBOPTIMAL_KHR:						return "VK_SUBOPTIMAL_KHR";
	case VK_ERROR_OUT_OF_DATE_KHR:				return "VK_ERROR_OUT_OF_DATE_KHR";
	case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:		return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
	case VK_ERROR_VALIDATION_FAILED_EXT:		return "VK_ERROR_VALIDATION_FAILED_EXT";
	default:									return "VK_ERROR_<unknown>";
	}
}

/*
================
VK_FindMemoryType

Pick a memory type from memProps satisfying typeBits and the requested property
flags.  ri.Error on failure -- a device with no matching heap cannot render.
================
*/
uint32_t VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags properties ) {
	uint32_t i;

	for ( i = 0; i < vk.memProps.memoryTypeCount; i++ ) {
		if ( ( typeBits & ( 1u << i ) ) &&
			 ( vk.memProps.memoryTypes[i].propertyFlags & properties ) == properties ) {
			return i;
		}
	}

	ri.Error( ERR_FATAL, "VK_FindMemoryType: no suitable memory type (bits 0x%x, props 0x%x)",
		typeBits, (unsigned)properties );
	return 0;
}

//==========================================================================

static qboolean VK_InstanceExtensionAvailable( const char *name ) {
	VkExtensionProperties	*props;
	uint32_t				count = 0, i;
	qboolean				found = qfalse;

	qvkEnumerateInstanceExtensionProperties( NULL, &count, NULL );
	if ( count == 0 ) {
		return qfalse;
	}
	props = (VkExtensionProperties *) ri.Hunk_AllocateTempMemory( sizeof( *props ) * count );
	qvkEnumerateInstanceExtensionProperties( NULL, &count, props );
	for ( i = 0; i < count; i++ ) {
		if ( !Q_stricmp( props[i].extensionName, name ) ) {
			found = qtrue;
			break;
		}
	}
	ri.Hunk_FreeTempMemory( props );
	return found;
}

static qboolean VK_InstanceLayerAvailable( const char *name ) {
	VkLayerProperties	*props;
	uint32_t			count = 0, i;
	qboolean			found = qfalse;

	qvkEnumerateInstanceLayerProperties( &count, NULL );
	if ( count == 0 ) {
		return qfalse;
	}
	props = (VkLayerProperties *) ri.Hunk_AllocateTempMemory( sizeof( *props ) * count );
	qvkEnumerateInstanceLayerProperties( &count, props );
	for ( i = 0; i < count; i++ ) {
		if ( !Q_stricmp( props[i].layerName, name ) ) {
			found = qtrue;
			break;
		}
	}
	ri.Hunk_FreeTempMemory( props );
	return found;
}

/*
================
VK_DebugCallback
================
*/
static VKAPI_ATTR VkBool32 VKAPI_CALL VK_DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT types,
	const VkDebugUtilsMessengerCallbackDataEXT *data,
	void *userData )
{
	if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) {
		ri.Printf( PRINT_ALL, "VK ERROR: %s\n", data->pMessage );
	} else if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ) {
		ri.Printf( PRINT_ALL, "VK WARN: %s\n", data->pMessage );
	}
	return VK_FALSE;
}

/*
================
VK_CreateInstance
================
*/
static qboolean VK_CreateInstance( void ) {
	VkApplicationInfo		appInfo;
	VkInstanceCreateInfo	createInfo;
	const char				*extensions[8];
	const char				*layers[1];
	uint32_t				extCount = 0;
	uint32_t				layerCount = 0;
	int						platformCount = 0;
	VkResult				res;
	cvar_t					*validation;

	// query the highest instance API version available
	vk.instanceApiVersion = VK_API_VERSION_1_0;
	if ( qvkEnumerateInstanceVersion ) {
		qvkEnumerateInstanceVersion( &vk.instanceApiVersion );
	}
	if ( vk.instanceApiVersion < VK_TARGET_API_VERSION ) {
		ri.Printf( PRINT_ALL, "...Vulkan loader reports %d.%d.%d, need >= 1.3\n",
			VK_API_VERSION_MAJOR( vk.instanceApiVersion ),
			VK_API_VERSION_MINOR( vk.instanceApiVersion ),
			VK_API_VERSION_PATCH( vk.instanceApiVersion ) );
		return qfalse;
	}

	// surface extensions: VK_KHR_surface + the platform's window-system surface
	// (win_vk.c appends "VK_KHR_win32_surface")
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extCount = 1;
	VKimp_GetRequiredInstanceExtensions( &extensions[extCount], &platformCount, 8 - extCount );
	extCount += platformCount;

	// optional debug + validation
	validation = ri.Cvar_Get( "r_vkValidation", "0", CVAR_ARCHIVE | CVAR_LATCH );
	vk.validation = qfalse;
	if ( validation->integer &&
		 VK_InstanceLayerAvailable( "VK_LAYER_KHRONOS_validation" ) &&
		 VK_InstanceExtensionAvailable( VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) ) {
		extensions[extCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		layers[layerCount++] = "VK_LAYER_KHRONOS_validation";
		vk.validation = qtrue;
		ri.Printf( PRINT_ALL, "...Vulkan validation layers enabled\n" );
	}

	memset( &appInfo, 0, sizeof( appInfo ) );
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Quake III Arena";
	appInfo.applicationVersion = VK_MAKE_VERSION( 1, 32, 0 );
	appInfo.pEngineName = "id Tech 3";
	appInfo.engineVersion = VK_MAKE_VERSION( 1, 32, 0 );
	appInfo.apiVersion = VK_TARGET_API_VERSION;

	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = extCount;
	createInfo.ppEnabledExtensionNames = extensions;
	createInfo.enabledLayerCount = layerCount;
	createInfo.ppEnabledLayerNames = layers;

	res = qvkCreateInstance( &createInfo, NULL, &vk.instance );
	if ( res != VK_SUCCESS ) {
		ri.Printf( PRINT_ALL, "...vkCreateInstance failed: %s\n", VK_ResultString( res ) );
		return qfalse;
	}

	return qtrue;
}

/*
================
VK_CreateDebugMessenger
================
*/
static void VK_CreateDebugMessenger( void ) {
	VkDebugUtilsMessengerCreateInfoEXT	info;
	PFN_vkCreateDebugUtilsMessengerEXT	create;

	if ( !vk.validation ) {
		return;
	}
	create = (PFN_vkCreateDebugUtilsMessengerEXT)
		qvkGetInstanceProcAddr( vk.instance, "vkCreateDebugUtilsMessengerEXT" );
	if ( !create ) {
		return;
	}

	memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
					   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
					   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	info.pfnUserCallback = VK_DebugCallback;

	create( vk.instance, &info, NULL, &vk.debugMessenger );
}

/*
================
VK_DeviceExtensionAvailable
================
*/
static qboolean VK_DeviceExtensionAvailable( VkPhysicalDevice device, const char *name ) {
	VkExtensionProperties	*props;
	uint32_t				count = 0, i;
	qboolean				found = qfalse;

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
VK_DeviceSupportsDynamicRendering

We target Vulkan 1.3 core dynamic rendering, so query the 1.3 feature struct.
================
*/
static qboolean VK_DeviceSupportsDynamicRendering( VkPhysicalDevice device ) {
	VkPhysicalDeviceVulkan13Features	vk13;
	VkPhysicalDeviceFeatures2			features2;

	memset( &vk13, 0, sizeof( vk13 ) );
	vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

	memset( &features2, 0, sizeof( features2 ) );
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &vk13;

	if ( !qvkGetPhysicalDeviceProperties2 ) {
		return qfalse;
	}
	// vkGetPhysicalDeviceFeatures2 shares the same loader slot as the KHR variant
	{
		PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)
			qvkGetInstanceProcAddr( vk.instance, "vkGetPhysicalDeviceFeatures2" );
		if ( !getFeatures2 ) {
			return qfalse;
		}
		getFeatures2( device, &features2 );
	}

	return ( vk13.dynamicRendering == VK_TRUE );
}

/*
================
VK_FindQueueFamilies

Locate a graphics-capable family and a present-capable family for the surface.
================
*/
static qboolean VK_FindQueueFamilies( VkPhysicalDevice device, uint32_t *graphics, uint32_t *present ) {
	VkQueueFamilyProperties	*families;
	uint32_t				count = 0, i;
	int						foundGraphics = -1;
	int						foundPresent = -1;

	qvkGetPhysicalDeviceQueueFamilyProperties( device, &count, NULL );
	if ( count == 0 ) {
		return qfalse;
	}
	families = (VkQueueFamilyProperties *) ri.Hunk_AllocateTempMemory( sizeof( *families ) * count );
	qvkGetPhysicalDeviceQueueFamilyProperties( device, &count, families );

	for ( i = 0; i < count; i++ ) {
		VkBool32 presentSupport = VK_FALSE;

		if ( ( families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) && foundGraphics < 0 ) {
			foundGraphics = (int)i;
		}
		qvkGetPhysicalDeviceSurfaceSupportKHR( device, i, vk.surface, &presentSupport );
		if ( presentSupport && foundPresent < 0 ) {
			foundPresent = (int)i;
		}
		// prefer a single family that does both
		if ( ( families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) && presentSupport ) {
			foundGraphics = (int)i;
			foundPresent = (int)i;
			break;
		}
	}

	ri.Hunk_FreeTempMemory( families );

	if ( foundGraphics < 0 || foundPresent < 0 ) {
		return qfalse;
	}
	*graphics = (uint32_t)foundGraphics;
	*present = (uint32_t)foundPresent;
	return qtrue;
}

/*
================
VK_SelectPhysicalDevice

Pick the best device that can present to our surface, supports the swapchain
extension, and supports 1.3 dynamic rendering.  Prefer discrete GPUs.
================
*/
static qboolean VK_SelectPhysicalDevice( void ) {
	VkPhysicalDevice	devices[16];
	uint32_t			count = 0, i;
	int					best = -1;
	int					bestScore = -1;

	qvkEnumeratePhysicalDevices( vk.instance, &count, NULL );
	if ( count == 0 ) {
		ri.Printf( PRINT_ALL, "...no Vulkan physical devices found\n" );
		return qfalse;
	}
	if ( count > 16 ) {
		count = 16;
	}
	qvkEnumeratePhysicalDevices( vk.instance, &count, devices );

	for ( i = 0; i < count; i++ ) {
		VkPhysicalDeviceProperties	props;
		uint32_t					gfx, present;
		int							score;

		qvkGetPhysicalDeviceProperties( devices[i], &props );

		if ( props.apiVersion < VK_TARGET_API_VERSION ) {
			continue;
		}
		if ( !VK_DeviceExtensionAvailable( devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME ) ) {
			continue;
		}
		if ( !VK_FindQueueFamilies( devices[i], &gfx, &present ) ) {
			continue;
		}
		if ( !VK_DeviceSupportsDynamicRendering( devices[i] ) ) {
			continue;
		}

		score = 0;
		if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) {
			score += 1000;
		} else if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) {
			score += 100;
		}
		score += props.limits.maxImageDimension2D / 1024;

		if ( score > bestScore ) {
			bestScore = score;
			best = (int)i;
		}
	}

	if ( best < 0 ) {
		ri.Printf( PRINT_ALL, "...no Vulkan 1.3 device with dynamic rendering + swapchain found\n" );
		return qfalse;
	}

	vk.physicalDevice = devices[best];
	qvkGetPhysicalDeviceProperties( vk.physicalDevice, &vk.devProps );
	qvkGetPhysicalDeviceMemoryProperties( vk.physicalDevice, &vk.memProps );
	qvkGetPhysicalDeviceFeatures( vk.physicalDevice, &vk.devFeatures );
	VK_FindQueueFamilies( vk.physicalDevice, &vk.graphicsFamily, &vk.presentFamily );

	ri.Printf( PRINT_ALL, "...selected GPU: %s\n", vk.devProps.deviceName );
	return qtrue;
}

/*
================
VK_CreateDevice
================
*/
static qboolean VK_CreateDevice( void ) {
	VkDeviceQueueCreateInfo		queueInfos[2];
	VkDeviceCreateInfo			createInfo;
	VkPhysicalDeviceVulkan13Features	vk13;
	VkPhysicalDeviceFeatures	enabledFeatures;
	const char					*deviceExtensions[1];
	float						priority = 1.0f;
	uint32_t					queueCount = 0;
	VkResult					res;

	memset( queueInfos, 0, sizeof( queueInfos ) );
	queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfos[0].queueFamilyIndex = vk.graphicsFamily;
	queueInfos[0].queueCount = 1;
	queueInfos[0].pQueuePriorities = &priority;
	queueCount = 1;
	if ( vk.presentFamily != vk.graphicsFamily ) {
		queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfos[1].queueFamilyIndex = vk.presentFamily;
		queueInfos[1].queueCount = 1;
		queueInfos[1].pQueuePriorities = &priority;
		queueCount = 2;
	}

	deviceExtensions[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

	// Q3 fixed-function pipeline needs almost nothing special; request the
	// handful of features we actually rely on.
	memset( &enabledFeatures, 0, sizeof( enabledFeatures ) );
	enabledFeatures.fillModeNonSolid = vk.devFeatures.fillModeNonSolid;	// r_showtris / GLS_POLYMODE_LINE
	enabledFeatures.samplerAnisotropy = vk.devFeatures.samplerAnisotropy;
	enabledFeatures.shaderClipDistance = vk.devFeatures.shaderClipDistance;	// mirror/portal gl_ClipDistance

	memset( &vk13, 0, sizeof( vk13 ) );
	vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	vk13.dynamicRendering = VK_TRUE;
	vk13.synchronization2 = VK_TRUE;

	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.pNext = &vk13;
	createInfo.queueCreateInfoCount = queueCount;
	createInfo.pQueueCreateInfos = queueInfos;
	createInfo.enabledExtensionCount = 1;
	createInfo.ppEnabledExtensionNames = deviceExtensions;
	createInfo.pEnabledFeatures = &enabledFeatures;

	res = qvkCreateDevice( vk.physicalDevice, &createInfo, NULL, &vk.device );
	if ( res != VK_SUCCESS ) {
		ri.Printf( PRINT_ALL, "...vkCreateDevice failed: %s\n", VK_ResultString( res ) );
		return qfalse;
	}

	return qtrue;
}

/*
================
VK_FillConfig

Populate glConfig from the live device so GfxInfo_f, the console and the UI all
behave.  Width/height/fullscreen/aspect were already set by VKimp_CreateWindow.
================
*/
static void VK_FillConfig( void ) {
	const char	*typeStr;

	switch ( vk.devProps.deviceType ) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:		typeStr = "Discrete GPU"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:	typeStr = "Integrated GPU"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:		typeStr = "Virtual GPU"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:				typeStr = "CPU"; break;
	default:										typeStr = "Other"; break;
	}

	Com_sprintf( glConfig.vendor_string, sizeof( glConfig.vendor_string ),
		"Vulkan vendor 0x%04x", vk.devProps.vendorID );
	Q_strncpyz( glConfig.renderer_string, vk.devProps.deviceName, sizeof( glConfig.renderer_string ) );
	Com_sprintf( glConfig.version_string, sizeof( glConfig.version_string ),
		"Vulkan %d.%d.%d (%s)",
		VK_API_VERSION_MAJOR( vk.devProps.apiVersion ),
		VK_API_VERSION_MINOR( vk.devProps.apiVersion ),
		VK_API_VERSION_PATCH( vk.devProps.apiVersion ),
		typeStr );
	Q_strncpyz( glConfig.extensions_string, "VK_KHR_swapchain VK_KHR_dynamic_rendering",
		sizeof( glConfig.extensions_string ) );

	glConfig.maxTextureSize = vk.devProps.limits.maxImageDimension2D;
	glConfig.maxActiveTextures = 2;					// we support 2 TMUs (NUM_TEXTURE_BUNDLES)
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;			// we gamma-correct on the CPU for GL parity
	glConfig.textureEnvAddAvailable = qtrue;
	glConfig.textureCompression = TC_NONE;
	glConfig.stereoEnabled = qfalse;
	glConfig.smpActive = qfalse;					// Vulkan backend is single-threaded for now

	// color/depth/stencil bits come from the chosen surface + depth format
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
}

//==========================================================================

/*
================
VK_DestroyAll

Full teardown, robust against partially-initialized state (every handle and
entry point is checked).  Mirrors GLimp_Shutdown by clearing glConfig/glState.
================
*/
static void VK_DestroyAll( void ) {
	if ( vk.device && qvkDeviceWaitIdle ) {
		qvkDeviceWaitIdle( vk.device );
	}

	if ( vk.device ) {
		VK_ShutdownPipelines();
		VK_ShutdownImageSystem();
		VK_DestroyStreamingBuffers();
	}

	VK_DestroyFrameResources();
	VK_DestroySwapchain();

	if ( vk.device && qvkDestroyDevice ) {
		qvkDestroyDevice( vk.device, NULL );
		vk.device = VK_NULL_HANDLE;
	}

	if ( vk.surface && qvkDestroySurfaceKHR ) {
		qvkDestroySurfaceKHR( vk.instance, vk.surface, NULL );
		vk.surface = VK_NULL_HANDLE;
	}

	if ( vk.debugMessenger && vk.instance ) {
		PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
			qvkGetInstanceProcAddr( vk.instance, "vkDestroyDebugUtilsMessengerEXT" );
		if ( destroy ) {
			destroy( vk.instance, vk.debugMessenger, NULL );
		}
		vk.debugMessenger = VK_NULL_HANDLE;
	}

	if ( vk.instance && qvkDestroyInstance ) {
		qvkDestroyInstance( vk.instance, NULL );
		vk.instance = VK_NULL_HANDLE;
	}

	VKimp_DestroyWindow();
	QVK_ClearProcAddresses();
	VKimp_FreeLibrary();

	memset( &vk, 0, sizeof( vk ) );
	memset( &glConfig, 0, sizeof( glConfig ) );
	memset( &glState, 0, sizeof( glState ) );
}

/*
================
VK_Init

Returns qfalse on any failure -- the caller falls back to OpenGL.
================
*/
qboolean VK_Init( void ) {
	// A vid_restart with destroyWindow == qfalse (e.g. the renderer refresh on map
	// load) leaves VK_Shutdown a no-op, so the device/swapchain are still alive.
	// Reuse them instead of building a SECOND device on top of the first -- exactly
	// like the GL backend reuses its context when glConfig.vidWidth != 0.  Creating
	// a second device/swapchain leaks the first AND breaks injected overlays
	// (MSI Afterburner/RTSS) which then submit across two devices -> DEVICE_LOST.
	if ( vk.initialized ) {
		ri.Printf( PRINT_ALL, "...Vulkan subsystem already up, reusing device\n" );
		return qtrue;
	}

	ri.Printf( PRINT_ALL, "Initializing Vulkan subsystem\n" );

	memset( &vk, 0, sizeof( vk ) );

	if ( !VKimp_LoadLibrary() ) {
		ri.Printf( PRINT_ALL, "...could not load vulkan-1.dll\n" );
		return qfalse;
	}
	if ( !QVK_InitGlobalFunctions() ) {
		VK_DestroyAll();
		return qfalse;
	}
	if ( !VKimp_CreateWindow() ) {
		VK_DestroyAll();
		return qfalse;
	}
	if ( !VK_CreateInstance() ) {
		VK_DestroyAll();
		return qfalse;
	}
	if ( !QVK_InitInstanceFunctions( vk.instance ) ) {
		VK_DestroyAll();
		return qfalse;
	}
	VK_CreateDebugMessenger();

	if ( !VKimp_CreateSurface( vk.instance, &vk.surface ) ) {
		ri.Printf( PRINT_ALL, "...could not create Vulkan surface\n" );
		VK_DestroyAll();
		return qfalse;
	}
	if ( !VK_SelectPhysicalDevice() ) {
		VK_DestroyAll();
		return qfalse;
	}
	if ( !VK_CreateDevice() ) {
		VK_DestroyAll();
		return qfalse;
	}
	if ( !QVK_InitDeviceFunctions( vk.device ) ) {
		VK_DestroyAll();
		return qfalse;
	}

	qvkGetDeviceQueue( vk.device, vk.graphicsFamily, 0, &vk.graphicsQueue );
	qvkGetDeviceQueue( vk.device, vk.presentFamily, 0, &vk.presentQueue );

	if ( !VK_CreateSwapchain() ) {
		VK_DestroyAll();
		return qfalse;
	}
	if ( !VK_CreateFrameResources() ) {
		VK_DestroyAll();
		return qfalse;
	}

	// resources for the textured draw path
	VK_CreateStreamingBuffers();
	VK_InitPipelines();			// descriptor/pipeline layouts + shaders (before any image)
	VK_InitImageSystem();		// descriptor pool + sampler cache

	// the shared stage iterators still issue GL client-array calls; make them inert
	VK_InstallInertGLProcs();

	VK_FillConfig();

	vk.initialized = qtrue;
	ri.Printf( PRINT_ALL, "...Vulkan subsystem initialized (%s)\n", vk.devProps.deviceName );
	return qtrue;
}

/*
================
VK_Shutdown

bk.Shutdown leaf.  Mirrors GLBE_Shutdown: only fully tears down when the caller
asks to destroy the window (vid_restart passes qtrue).
================
*/
void VK_Shutdown( qboolean destroyWindow ) {
	if ( !destroyWindow ) {
		return;
	}
	ri.Printf( PRINT_ALL, "Shutting down Vulkan subsystem\n" );
	VK_DestroyAll();
}

/*
================
VK_GfxInfo

bk.GfxInfo leaf.
================
*/
void VK_GfxInfo( void ) {
	ri.Printf( PRINT_ALL, "\nVK_VENDOR: %s\n", glConfig.vendor_string );
	ri.Printf( PRINT_ALL, "VK_DEVICE: %s\n", glConfig.renderer_string );
	ri.Printf( PRINT_ALL, "VK_VERSION: %s\n", glConfig.version_string );
	ri.Printf( PRINT_ALL, "VK_MAX_TEXTURE_SIZE: %d\n", glConfig.maxTextureSize );
	ri.Printf( PRINT_ALL, "MODE: %d, %d x %d %s\n", r_mode->integer,
		glConfig.vidWidth, glConfig.vidHeight,
		glConfig.isFullscreen ? "fullscreen" : "windowed" );
	ri.Printf( PRINT_ALL, "GAMMA: software w/ %d overbright bits\n", tr.overbrightBits );
	ri.Printf( PRINT_ALL, "texturemode: %s\n", r_textureMode->string );
	ri.Printf( PRINT_ALL, "picmip: %d\n", r_picmip->integer );
	ri.Printf( PRINT_ALL, "validation: %s\n", vk.validation ? "enabled" : "disabled" );
	ri.Printf( PRINT_ALL, "present mode: %s\n",
		vk.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)" :
		vk.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "IMMEDIATE" );
}
