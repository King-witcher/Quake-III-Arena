#version 450
//
// accum.frag -- frame-multisampling (temporal accumulation) helper.  Samples a
// texture and outputs it scaled by pc.scale.  Used by two passes:
//   * accumulate: adds each rendered frame into the float16 sum buffer
//     (scale = 1, additive blend, RGB-only color write mask).
//   * resolve: writes the averaged result (scale = 1 / frames) into the held image
//     that is blitted to the swapchain.
//
layout( push_constant ) uniform PushConstants {
	float scale;		// 1.0 for accumulate, 1.0/frames for resolve
} pc;

layout( set = 0, binding = 0 ) uniform sampler2D u_tex;

layout( location = 0 ) in vec2 frag_uv;
layout( location = 0 ) out vec4 out_color;

void main() {
	out_color = vec4( texture( u_tex, frag_uv ).rgb * pc.scale, 1.0 );
}
