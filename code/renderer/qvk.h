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
// qvk.h -- Vulkan function-pointer indirection.
//
// Mirrors qgl.h / win_qgl.c: the Vulkan loader (vulkan-1.dll) is loaded
// dynamically at runtime via LoadLibrary + vkGetInstanceProcAddr, exactly
// like the OpenGL ICD.  This means the engine keeps building as a 32-bit
// Win32 binary and needs no vulkan-1.lib import library (only the SDK
// headers, included below).  Every entry point we use is reached through a
// qvk* function pointer.
//
#ifndef __QVK_H__
#define __QVK_H__

// We resolve everything through function pointers ourselves, so suppress the
// prototype declarations that vulkan.h would otherwise emit.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

//
// The X-macro lists below enumerate exactly the entry points the renderer
// uses, grouped by the level at which they must be resolved:
//
//   GLOBAL   - resolved with vkGetInstanceProcAddr( NULL, name )
//   INSTANCE - resolved with vkGetInstanceProcAddr( instance, name )
//   DEVICE   - resolved with vkGetDeviceProcAddr( device, name )
//
// Adding a new entry point is a one-line change to the appropriate list.
//

#define QVK_GLOBAL_FUNCTION_LIST(X)               \
	X( vkCreateInstance )                         \
	X( vkEnumerateInstanceExtensionProperties )   \
	X( vkEnumerateInstanceLayerProperties )       \
	X( vkEnumerateInstanceVersion )

#define QVK_INSTANCE_FUNCTION_LIST(X)                   \
	X( vkDestroyInstance )                              \
	X( vkEnumeratePhysicalDevices )                     \
	X( vkGetPhysicalDeviceProperties )                  \
	X( vkGetPhysicalDeviceProperties2 )                 \
	X( vkGetPhysicalDeviceFeatures )                    \
	X( vkGetPhysicalDeviceMemoryProperties )            \
	X( vkGetPhysicalDeviceQueueFamilyProperties )       \
	X( vkGetPhysicalDeviceFormatProperties )            \
	X( vkEnumerateDeviceExtensionProperties )           \
	X( vkCreateDevice )                                 \
	X( vkGetDeviceProcAddr )                            \
	X( vkDestroySurfaceKHR )                            \
	X( vkGetPhysicalDeviceSurfaceSupportKHR )           \
	X( vkGetPhysicalDeviceSurfaceCapabilitiesKHR )      \
	X( vkGetPhysicalDeviceSurfaceFormatsKHR )           \
	X( vkGetPhysicalDeviceSurfacePresentModesKHR )

#define QVK_DEVICE_FUNCTION_LIST(X)             \
	X( vkDestroyDevice )                        \
	X( vkGetDeviceQueue )                       \
	X( vkDeviceWaitIdle )                       \
	X( vkQueueSubmit )                          \
	X( vkQueueWaitIdle )                        \
	X( vkQueuePresentKHR )                      \
	X( vkCreateSwapchainKHR )                   \
	X( vkDestroySwapchainKHR )                  \
	X( vkGetSwapchainImagesKHR )               \
	X( vkAcquireNextImageKHR )                  \
	X( vkCreateImageView )                      \
	X( vkDestroyImageView )                     \
	X( vkCreateImage )                          \
	X( vkDestroyImage )                         \
	X( vkGetImageMemoryRequirements )           \
	X( vkBindImageMemory )                      \
	X( vkAllocateMemory )                       \
	X( vkFreeMemory )                           \
	X( vkMapMemory )                            \
	X( vkUnmapMemory )                          \
	X( vkFlushMappedMemoryRanges )              \
	X( vkCreateBuffer )                         \
	X( vkDestroyBuffer )                        \
	X( vkGetBufferMemoryRequirements )          \
	X( vkBindBufferMemory )                     \
	X( vkCreateCommandPool )                    \
	X( vkDestroyCommandPool )                   \
	X( vkResetCommandPool )                     \
	X( vkAllocateCommandBuffers )               \
	X( vkFreeCommandBuffers )                   \
	X( vkBeginCommandBuffer )                   \
	X( vkEndCommandBuffer )                     \
	X( vkResetCommandBuffer )                   \
	X( vkCreateSemaphore )                      \
	X( vkDestroySemaphore )                     \
	X( vkCreateFence )                          \
	X( vkDestroyFence )                         \
	X( vkWaitForFences )                        \
	X( vkResetFences )                          \
	X( vkCmdPipelineBarrier )                   \
	X( vkCmdBeginRendering )                    \
	X( vkCmdEndRendering )                      \
	X( vkCmdClearColorImage )                   \
	X( vkCmdClearAttachments )                  \
	X( vkCmdSetViewport )                       \
	X( vkCmdSetScissor )                        \
	X( vkCmdSetDepthBias )                      \
	X( vkCmdBindPipeline )                      \
	X( vkCmdBindDescriptorSets )                \
	X( vkCmdBindVertexBuffers )                 \
	X( vkCmdBindIndexBuffer )                   \
	X( vkCmdPushConstants )                     \
	X( vkCmdDraw )                              \
	X( vkCmdDrawIndexed )                       \
	X( vkCmdCopyBufferToImage )                 \
	X( vkCmdCopyImageToBuffer )                 \
	X( vkCreateSampler )                        \
	X( vkDestroySampler )                       \
	X( vkCreateDescriptorSetLayout )            \
	X( vkDestroyDescriptorSetLayout )           \
	X( vkCreateDescriptorPool )                 \
	X( vkDestroyDescriptorPool )                \
	X( vkAllocateDescriptorSets )               \
	X( vkUpdateDescriptorSets )                 \
	X( vkCreatePipelineLayout )                 \
	X( vkDestroyPipelineLayout )                \
	X( vkCreateShaderModule )                   \
	X( vkDestroyShaderModule )                  \
	X( vkCreateGraphicsPipelines )              \
	X( vkDestroyPipeline )                      \
	X( vkCreatePipelineCache )                  \
	X( vkDestroyPipelineCache )                 \
	X( vkGetPipelineCacheData )

// The bootstrap entry point, obtained from vulkan-1.dll with GetProcAddress
// in the platform layer (win_vk.c).  Everything else hangs off of it.
extern PFN_vkGetInstanceProcAddr	qvkGetInstanceProcAddr;

// Declare every pointer: extern PFN_vkXxx qvkXxx;
#define QVK_DECLARE_FUNCTION(name)	extern PFN_##name q##name;
QVK_GLOBAL_FUNCTION_LIST( QVK_DECLARE_FUNCTION )
QVK_INSTANCE_FUNCTION_LIST( QVK_DECLARE_FUNCTION )
QVK_DEVICE_FUNCTION_LIST( QVK_DECLARE_FUNCTION )
#undef QVK_DECLARE_FUNCTION

// Resolve each group.  Return qfalse if any required pointer is missing.
qboolean QVK_InitGlobalFunctions( void );
qboolean QVK_InitInstanceFunctions( VkInstance instance );
qboolean QVK_InitDeviceFunctions( VkDevice device );
void     QVK_ClearProcAddresses( void );

#endif // __QVK_H__
