#version 450
//
// single.vert -- Quake 3 Vulkan backend vertex shader (shared by single + multi).
//
// Q3 computes every per-vertex value (deforms, rgbGen, alphaGen, tcGen, tcMod,
// fog, dlight) on the CPU into the tess arrays, so the GPU only transforms by
// the MVP and passes the interpolants through.  A world-space clip plane drives
// gl_ClipDistance for mirror/portal views (clipPlane = 0 means no clipping).
//
layout( push_constant ) uniform PushConstants {
	mat4 mvp;
	vec4 clipPlane;		// world-space plane (n.xyz, -dist); 0 = disabled
} pc;

layout( location = 0 ) in vec3 in_position;
layout( location = 1 ) in vec4 in_color;
layout( location = 2 ) in vec2 in_texCoord0;
layout( location = 3 ) in vec2 in_texCoord1;

layout( location = 0 ) out vec4 frag_color;
layout( location = 1 ) out vec2 frag_texCoord0;
layout( location = 2 ) out vec2 frag_texCoord1;

out gl_PerVertex {
	vec4  gl_Position;
	float gl_ClipDistance[1];
};

void main() {
	gl_Position       = pc.mvp * vec4( in_position, 1.0 );
	gl_ClipDistance[0] = dot( vec4( in_position, 1.0 ), pc.clipPlane );
	frag_color        = in_color;
	frag_texCoord0    = in_texCoord0;
	frag_texCoord1    = in_texCoord1;
}
