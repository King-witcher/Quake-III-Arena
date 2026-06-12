#!/usr/bin/env bash
# compile.sh -- (re)generate code/renderer/vk_spv.h from the GLSL shaders here.
#
# vk_spv.h embeds each shader's SPIR-V as a C uint32_t array, named
# vk_spv_<file_with_dots_as_underscores> (e.g. rt_light.comp -> vk_spv_rt_light_comp).
# The Vulkan backend creates VkShaderModules straight from these arrays, so there is
# no runtime shader compilation and no build-time toolchain dependency.
#
# Usage:   bash compile.sh            # rebuild the whole header from *.vert/.frag/.comp
#          bash compile.sh rt_light.comp   # rebuild header for just the listed shaders,
#                                          # preserving every other array already present
#
# Requires the Vulkan SDK's glslc on PATH or at $VULKAN_SDK/Bin.  Ray-query shaders
# need --target-env=vulkan1.3 (rayQueryEXT is Vulkan 1.3 + VK_KHR_ray_query).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/../vk_spv.h"
glslc="${GLSLC:-${VULKAN_SDK:-}/Bin/glslc.exe}"
[ -x "$glslc" ] || glslc="glslc"

# full set, in a stable order
all_shaders=( single.vert single.frag multi.frag fullscreen.vert fxaa.frag downsample.frag rt_light.comp )

# pick the subset to (re)compile
if [ "$#" -gt 0 ]; then
	targets=( "$@" )
else
	targets=( "${all_shaders[@]}" )
fi

symname() { echo "vk_spv_${1//./_}"; }

# compile one shader to a "{0x..,..}" C-initialiser body on stdout
compile_one() {
	local f="$1" tmp
	tmp="$(mktemp)"
	"$glslc" --target-env=vulkan1.3 -O "$here/$f" -mfmt=c -o "$tmp"
	cat "$tmp"
	rm -f "$tmp"
}

tmpout="$(mktemp)"
{
	echo "//"
	echo "// vk_spv.h -- precompiled SPIR-V modules embedded as 32-bit word arrays."
	echo "// GENERATED from renderer/shaders/*.vert/.frag/.comp by glslc (-mfmt=c)."
	echo "// Regenerate with renderer/shaders/compile.sh."
	echo "//"
	echo "#ifndef __VK_SPV_H__"
	echo "#define __VK_SPV_H__"
	echo ""
	for f in "${all_shaders[@]}"; do
		sym="$(symname "$f")"
		echo "static const uint32_t ${sym}[] ="
		recompile=0
		for t in "${targets[@]}"; do [ "$t" = "$f" ] && recompile=1; done
		if [ "$recompile" = "1" ]; then
			compile_one "$f"
		else
			# preserve the existing array body verbatim from the current header
			awk -v s="static const uint32_t ${sym}[] =" '
				$0==s {grab=1; next}
				grab && /^;/ {exit}
				grab {print}
			' "$out"
		fi
		echo ";"
		echo ""
	done
	echo "#endif // __VK_SPV_H__"
} > "$tmpout"

mv "$tmpout" "$out"
echo "wrote $out"
