#version 450
//
// fullscreen.vert -- emits a single oversized triangle covering the whole screen
// from gl_VertexIndex (draw 3 vertices, no vertex buffer).  Used by the post-
// processing passes (FXAA).  uv is 0..1 across the visible area, top-left origin,
// matching the offscreen image storage, so the post pass must run with a normal
// POSITIVE-height viewport (no Y flip) for a 1:1 sample->screen mapping.
//
layout( location = 0 ) out vec2 frag_uv;

void main() {
	vec2 uv = vec2( ( gl_VertexIndex << 1 ) & 2, gl_VertexIndex & 2 );
	frag_uv = uv;
	gl_Position = vec4( uv * 2.0 - 1.0, 0.0, 1.0 );
}
