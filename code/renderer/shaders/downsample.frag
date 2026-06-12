#version 450
//
// downsample.frag -- exact box downsample of the supersampled offscreen image for
// SSAA.  Each output (display) pixel averages a factor x factor block of source
// texels, so the result is a true N-times supersample (a single LINEAR blit would
// only average a 2x2 neighbourhood regardless of the factor).
//
layout( push_constant ) uniform PushConstants {
	vec2 invSrcRes;		// 1.0 / supersampled offscreen resolution
	int  factor;		// integer supersample factor per axis (e.g. 8)
} pc;

layout( set = 0, binding = 0 ) uniform sampler2D u_tex;

layout( location = 0 ) in vec2 frag_uv;
layout( location = 0 ) out vec4 out_color;

void main() {
	// gl_FragCoord is the destination (display) pixel centre; its integer part is the
	// pixel index, whose source block starts at index*factor.
	ivec2 base = ivec2( gl_FragCoord.xy ) * pc.factor;
	vec3  sum = vec3( 0.0 );
	int   x, y;

	for ( y = 0; y < pc.factor; y++ ) {
		for ( x = 0; x < pc.factor; x++ ) {
			sum += texture( u_tex, ( vec2( base + ivec2( x, y ) ) + 0.5 ) * pc.invSrcRes ).rgb;
		}
	}

	out_color = vec4( sum / float( pc.factor * pc.factor ), 1.0 );
}
