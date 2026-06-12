#version 450
//
// fxaa.frag -- compact FXAA (Timothy Lottes' FXAA3, console/Geeks3D variant).
// Edge-directed blur of the offscreen scene color.  invRes = 1 / render size.
//
layout( push_constant ) uniform PushConstants {
	vec2 invRes;		// 1.0 / offscreen resolution
} pc;

layout( set = 0, binding = 0 ) uniform sampler2D u_tex;

layout( location = 0 ) in vec2 frag_uv;
layout( location = 0 ) out vec4 out_color;

float luma( vec3 c ) { return dot( c, vec3( 0.299, 0.587, 0.114 ) ); }

void main() {
	vec2 inv = pc.invRes;

	vec3  rgbM  = texture( u_tex, frag_uv ).rgb;
	float lumaM  = luma( rgbM );
	float lumaNW = luma( texture( u_tex, frag_uv + vec2( -1.0, -1.0 ) * inv ).rgb );
	float lumaNE = luma( texture( u_tex, frag_uv + vec2(  1.0, -1.0 ) * inv ).rgb );
	float lumaSW = luma( texture( u_tex, frag_uv + vec2( -1.0,  1.0 ) * inv ).rgb );
	float lumaSE = luma( texture( u_tex, frag_uv + vec2(  1.0,  1.0 ) * inv ).rgb );

	float lumaMin = min( lumaM, min( min( lumaNW, lumaNE ), min( lumaSW, lumaSE ) ) );
	float lumaMax = max( lumaM, max( max( lumaNW, lumaNE ), max( lumaSW, lumaSE ) ) );

	// skip pixels with little local contrast (no visible edge)
	if ( lumaMax - lumaMin < max( 0.0312, lumaMax * 0.125 ) ) {
		out_color = vec4( rgbM, 1.0 );
		return;
	}

	vec2 dir;
	dir.x = -( ( lumaNW + lumaNE ) - ( lumaSW + lumaSE ) );
	dir.y =  ( ( lumaNW + lumaSW ) - ( lumaNE + lumaSE ) );

	float dirReduce = max( ( lumaNW + lumaNE + lumaSW + lumaSE ) * ( 0.25 * 0.125 ), 1.0 / 128.0 );
	float rcpDirMin = 1.0 / ( min( abs( dir.x ), abs( dir.y ) ) + dirReduce );
	dir = clamp( dir * rcpDirMin, vec2( -8.0 ), vec2( 8.0 ) ) * inv;

	vec3 rgbA = 0.5 * (
		texture( u_tex, frag_uv + dir * ( 1.0 / 3.0 - 0.5 ) ).rgb +
		texture( u_tex, frag_uv + dir * ( 2.0 / 3.0 - 0.5 ) ).rgb );
	vec3 rgbB = rgbA * 0.5 + 0.25 * (
		texture( u_tex, frag_uv + dir * -0.5 ).rgb +
		texture( u_tex, frag_uv + dir *  0.5 ).rgb );

	float lumaB = luma( rgbB );
	if ( lumaB < lumaMin || lumaB > lumaMax ) {
		out_color = vec4( rgbA, 1.0 );
	} else {
		out_color = vec4( rgbB, 1.0 );
	}
}
