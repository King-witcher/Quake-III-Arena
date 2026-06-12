#version 450
//
// single.frag -- single-texture modulate with optional alpha test.
//
// out = vertexColor * texture(tmu0, uv0).  Alpha test is a specialization
// constant so the three Q3 modes do not multiply the pipeline/shader count at
// runtime; thresholds match GL (GLS_ATEST_GT_0 / _LT_80 / _GE_80).
//
layout( constant_id = 0 ) const int c_alphaTest = 0;	// 0 none, 1 GT0, 2 LT80, 3 GE80

layout( set = 0, binding = 0 ) uniform sampler2D u_tex0;

layout( location = 0 ) in vec4 frag_color;
layout( location = 1 ) in vec2 frag_texCoord0;
layout( location = 2 ) in vec2 frag_texCoord1;

layout( location = 0 ) out vec4 out_color;

void main() {
	vec4 c = frag_color * texture( u_tex0, frag_texCoord0 );

	if ( c_alphaTest == 1 ) {			// GL_GREATER 0.0  -> keep a > 0
		if ( c.a <= 0.0 ) discard;
	} else if ( c_alphaTest == 2 ) {	// GL_LESS 0.5     -> keep a < 0.5
		if ( c.a >= 0.5 ) discard;
	} else if ( c_alphaTest == 3 ) {	// GL_GEQUAL 0.5   -> keep a >= 0.5
		if ( c.a < 0.5 ) discard;
	}

	out_color = c;
}
