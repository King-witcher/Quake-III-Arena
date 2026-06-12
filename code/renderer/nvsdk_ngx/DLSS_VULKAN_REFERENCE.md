# NVIDIA NGX DLSS — Vulkan Integration Reference (C, Windows x64)

> Implementation reference used by `vk_dlss.c`. Grounded in the real SDK headers
> from https://github.com/NVIDIA/DLSS (`include/`) and the DLSS Programming Guide.
> See the bottom of this file for how to drop in the SDK to activate DLSS.

## Headers / include set (Vulkan first)
```c
#include <vulkan/vulkan.h>
#include "nvsdk_ngx.h"             /* pulls in nvsdk_ngx_defs.h + nvsdk_ngx_params.h */
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"
```
Link `nvsdk_ngx_s.lib` (or `_d.lib`) from `lib/Windows_x86_64/`. Ship `nvngx_dlss.dll`
next to the exe and point `NVSDK_NGX_FeatureCommonInfo::PathListInfo` at that dir.
**DLSS is x64-only, Vulkan >= 1.1.**

## Extension query (BEFORE VkInstance/VkDevice)
`NVSDK_NGX_VULKAN_RequiredExtensions(&instCnt,&instExts,&devCnt,&devExts)` — static
lists, no init needed. Add the returned names to the instance/device create-infos.

## Init
`NVSDK_NGX_VULKAN_Init_with_ProjectID(projGuid, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ver,
  appDataPathW, instance, phys, device, NULL, NULL, &commonInfo, NVSDK_NGX_Version_API)`

## Capability params + availability
`NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps)` then
`NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail)`.

## Optimal render settings
`NGX_DLSS_GET_OPTIMAL_SETTINGS(caps, outW, outH, perfQ, &rW,&rH, &maxW,&maxH,&minW,&minH,&sharp)`

`NVSDK_NGX_PerfQuality_Value`: MaxPerf=0 (Performance), Balanced=1, MaxQuality=2 (Quality),
UltraPerformance=3, UltraQuality=4, DLAA=5. Render = output / {3.0, 2.0, 1.724, 1.5, -, 1.0}.

## Create feature (needs an open, submitted cmd buffer)
`NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1,1, &handle, params, &createParams)`
`NVSDK_NGX_DLSS_Create_Params{ Feature{InWidth=rW,InHeight=rH,InTargetWidth=outW,
  InTargetHeight=outH,InPerfQualityValue=perfQ}, InFeatureCreateFlags, InEnableOutputSubrects }`
Flags: MVLowRes(1<<1) for render-res MVs, DepthInverted(1<<3) for reversed-Z,
AutoExposure(1<<6) when no exposure texture, IsHDR(1<<0).

## Per-frame evaluate
`NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, handle, params, &evalParams)`
`NVSDK_NGX_VK_DLSS_Eval_Params{ Feature{pInColor,pInOutput,InSharpness}, pInDepth,
  pInMotionVectors, InJitterOffsetX/Y, InRenderSubrectDimensions{Width,Height},
  InReset, InMVScaleX/Y(=1.0), ... }`
Wrap images: `NVSDK_NGX_Create_ImageView_Resource_VK(view, image, subresRange, format,
  w, h, readWrite)` — readWrite=false for inputs, true for the output.

## Resource state
Inputs (color render-res pre-tonemap, depth, MVs): `SAMPLED_BIT`,
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. Output (display-res): `STORAGE_BIT`,
`VK_IMAGE_LAYOUT_GENERAL`. Simplest correct: transition all to GENERAL around evaluate.

## Jitter
Halton(2,3), phases = 8*(outW/rW)^2. Apply to projection:
`proj[2][0] += jitterX_ndc; proj[2][1] += jitterY_ndc;` Pass `InJitterOffsetX/Y` in
render-res pixels, [-0.5,0.5], same sign convention as MVs.

## Shutdown
`NVSDK_NGX_VULKAN_ReleaseFeature(handle)` -> `NVSDK_NGX_VULKAN_DestroyParameters(params)`
(only Allocate'd maps) -> `NVSDK_NGX_VULKAN_Shutdown1(device)`. `vkDeviceWaitIdle` first.

## Result macros
`NVSDK_NGX_SUCCEED(v)`/`NVSDK_NGX_FAILED(v)` — success is 0x1, failure space masked 0xFFF00000.

---

## Activating DLSS in this build
This engine compiles a complete DLSS integration (`renderer/vk_dlss.c`) that is gated
behind the `USE_DLSS` preprocessor define. Out of the box `USE_DLSS` is **off**, so the
engine builds and runs with no NGX dependency and the DLSS menu options fall back to
native rendering with a console notice.

To activate real DLSS:
1. Download the NVIDIA DLSS SDK (https://github.com/NVIDIA/DLSS) and copy its
   `include/` headers into `code/renderer/nvsdk_ngx/include/`.
2. Copy `lib/Windows_x86_64/x86_64/nvsdk_ngx_s.lib` into `code/renderer/nvsdk_ngx/lib/`.
3. Copy `lib/Windows_x86_64/rel/nvngx_dlss.dll` next to the built `quake3.exe`
   (and into `run/` for the dev launch).
4. Build the **x64** config with `USE_DLSS` defined (the quake3 x64 project defines it
   automatically once `nvsdk_ngx/include/nvsdk_ngx.h` is present — see
   `vk_dlss.c` top comment).
