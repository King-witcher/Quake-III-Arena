#version 450
//
// lit.vert -- Blinn-Phong world-surface vertex shader (r_perPixelLighting).
//
// Same transform as single.vert, but also forwards the WORLD-SPACE position so
// the fragment stage can build view/light vectors and reconstruct the surface
// normal from screen-space derivatives.  Q3 world surfaces are submitted in world
// space (the world "model matrix" is the view-only identity), so in_position is
// already the world position.
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
layout( location = 3 ) out vec3 frag_worldPos;

out gl_PerVertex {
	vec4  gl_Position;
	float gl_ClipDistance[1];
};

void main() {
	gl_Position        = pc.mvp * vec4( in_position, 1.0 );
	gl_ClipDistance[0] = dot( vec4( in_position, 1.0 ), pc.clipPlane );
	frag_color         = in_color;
	frag_texCoord0     = in_texCoord0;
	frag_texCoord1     = in_texCoord1;
	frag_worldPos      = in_position;
}
