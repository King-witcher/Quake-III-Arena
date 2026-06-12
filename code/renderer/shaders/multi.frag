#version 450
//
// multi.frag -- two-texture combine (Q3 multitexture collapse).
//
// Reproduces the fixed-function texenv on unit 1: out = combine(vertexColor*tex0, tex1)
// with combine selected by a spec constant (GL_MODULATE / GL_ADD / GL_REPLACE).
// Used for collapsed diffuse x lightmap surfaces (MODULATE) and additive pairs.
// Vertex stage is single.vert (it already forwards both texcoord sets).
//
layout( constant_id = 0 ) const int c_alphaTest = 0;	// 0 none, 1 GT0, 2 LT80, 3 GE80
layout( constant_id = 1 ) const int c_combine   = 0;	// 0 MODULATE, 1 ADD, 2 REPLACE

layout( set = 0, binding = 0 ) uniform sampler2D u_tex0;
layout( set = 1, binding = 0 ) uniform sampler2D u_tex1;

layout( location = 0 ) in vec4 frag_color;
layout( location = 1 ) in vec2 frag_texCoord0;
layout( location = 2 ) in vec2 frag_texCoord1;

layout( location = 0 ) out vec4 out_color;

void main() {
	vec4 a = frag_color * texture( u_tex0, frag_texCoord0 );	// unit 0 output
	vec4 b = texture( u_tex1, frag_texCoord1 );				// unit 1 texture
	vec4 c;

	if ( c_combine == 1 ) {			// GL_ADD
		c = a + b;
	} else if ( c_combine == 2 ) {	// GL_REPLACE (unit 1 only, e.g. r_lightmap)
		c = b;
	} else {						// GL_MODULATE
		c = a * b;
	}

	if ( c_alphaTest == 1 ) {
		if ( c.a <= 0.0 ) discard;
	} else if ( c_alphaTest == 2 ) {
		if ( c.a >= 0.5 ) discard;
	} else if ( c_alphaTest == 3 ) {
		if ( c.a < 0.5 ) discard;
	}

	out_color = c;
}
