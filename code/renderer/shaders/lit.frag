#version 450
//
// lit.frag -- diffuse*lightmap base (Q3 multitexture MODULATE) PLUS the Blinn-Phong
// specular layer the vanilla engine lacks.  Only lightmapped opaque WORLD surfaces
// reach this shader (selected in VK_DrawElements under r_perPixelLighting).
//
// The lightmap already carries all the static *diffuse* lighting, and the engine's
// dlight pass adds dynamic *diffuse*; this shader only ADDS specular, so nothing is
// double-counted.  Specular is from DYNAMIC LIGHTS ONLY -- their distance attenuation
// localises the highlight, so flat world faces (Q3 has no normal maps) don't glow
// uniformly the way a single directional light would; a static scene matches vanilla.
// The surface normal is reconstructed per-pixel from the world-position derivatives
// (exact for the BSP's planar faces -> no per-vertex normal needed).  The specular
// contribution is scaled by identityLight so it shares the lightmap's pre-gamma range
// and the hardware gamma ramp lifts both identically.
//
#define MAX_DLIGHTS 32

layout( constant_id = 0 ) const int c_alphaTest = 0;	// 0 none, 1 GT0, 2 LT80, 3 GE80
layout( constant_id = 1 ) const int c_combine   = 0;	// 0 MODULATE (world lightmap), 1 ADD, 2 REPLACE

layout( set = 0, binding = 0 ) uniform sampler2D u_tex0;	// diffuse
layout( set = 1, binding = 0 ) uniform sampler2D u_tex1;	// lightmap

layout( set = 2, binding = 0 ) uniform LightData {
	vec4 viewOrigin;					// xyz world camera
	vec4 params;						// x=identityLight y=specExponent z=specScale w=numDlights
	vec4 dlightPos[MAX_DLIGHTS];		// xyz world origin, w=radius
	vec4 dlightColor[MAX_DLIGHTS];		// rgb colour 0..1
} u_light;

layout( location = 0 ) in vec4 frag_color;
layout( location = 1 ) in vec2 frag_texCoord0;
layout( location = 2 ) in vec2 frag_texCoord1;
layout( location = 3 ) in vec3 frag_worldPos;

layout( location = 0 ) out vec4 out_color;

void main() {
	vec4 tex0 = texture( u_tex0, frag_texCoord0 );			// diffuse texture = surface albedo
	vec4 a = frag_color * tex0;								// diffuse * vertexColor
	vec4 b = texture( u_tex1, frag_texCoord1 );				// lightmap
	vec4 base;

	if ( c_combine == 1 )      base = a + b;
	else if ( c_combine == 2 ) base = b;
	else                       base = a * b;	// GL_MODULATE: the world case

	// Reconstruct the geometric normal from the world-position derivatives BEFORE
	// any discard, so the derivatives are taken in uniform control flow.  Guard the
	// normalize: edge-on / sliver triangles give a ~zero cross product (NaN) -- fall
	// back to the view vector so those pixels just get no meaningful specular.
	vec3 V  = normalize( u_light.viewOrigin.xyz - frag_worldPos );
	vec3 ng = cross( dFdx( frag_worldPos ), dFdy( frag_worldPos ) );
	float nl = length( ng );
	vec3 N  = ( nl > 1e-8 ) ? ng / nl : V;
	if ( dot( N, V ) < 0.0 ) {
		N = -N;
	}

	if ( c_alphaTest == 1 )      { if ( base.a <= 0.0 ) discard; }
	else if ( c_alphaTest == 2 ) { if ( base.a >= 0.5 ) discard; }
	else if ( c_alphaTest == 3 ) { if ( base.a <  0.5 ) discard; }

	float exponent  = max( u_light.params.y, 1.0 );
	float specScale = u_light.params.z;
	vec3  spec      = vec3( 0.0 );

	// Specular comes ONLY from dynamic lights (rockets/plasma/etc.).  Their distance
	// attenuation localises the highlight, so flat world faces don't glow uniformly the
	// way a single directional light would.  Static scenes therefore match vanilla;
	// the diffuse of these dlights is still the engine's separate additive pass.
	int n = int( u_light.params.w );
	for ( int i = 0; i < n; ++i ) {
		vec3  toL = u_light.dlightPos[i].xyz - frag_worldPos;
		float r   = u_light.dlightPos[i].w;
		float d2  = dot( toL, toL );
		float d   = sqrt( d2 );
		vec3  Lp  = toL / max( d, 0.0001 );

		// Infinite-range physical falloff, no radius cut.  A light of influence radius r
		// has intrinsic intensity ~r^2 and attenuates by 1/d^2, so atten = r^2 / d^2.
		// This normalises to exactly 1.0 at d == r (matching the old peak so specScale
		// keeps its meaning), grows brighter closer in, and fades toward -- but never
		// reaches -- zero with distance, so the light reaches everywhere like real life.
		// d^2 is clamped to 1 unit to kill the 1/0 singularity at the source.
		float atten = ( r * r ) / max( d2, 1.0 );

		vec3  H = normalize( Lp + V );
		spec += u_light.dlightColor[i].rgb * pow( max( dot( N, H ), 0.0 ), exponent ) * atten;
	}

	// Modulate the specular by the surface albedo: a black texel reflects nothing, and
	// a red texel lit by a white light reflects red (material-tinted reflection).
	out_color = vec4( base.rgb + 2.0f * spec * tex0.rgb * specScale * u_light.params.x, base.a );
}
