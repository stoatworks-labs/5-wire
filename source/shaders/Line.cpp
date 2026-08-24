#include "../Shaders.h"

namespace fivewire::shaders
{
/**
    The cable.

    One convolution with the response from Cable.cpp, and a fractional offset
    per conductor.

    The kernel is causal and one-sided, so a bright edge smears to the RIGHT
    and only to the right. That is not a stylistic choice about which way looks
    better -- it is the difference between this and a blur, and it is the thing
    an audience recognises without being able to name.

    `Split` chooses between one fetch per tap and three. Five separate coaxes
    cut from the same drum are the same length to within a nanosecond, so the
    three-fetch path is switched on only when the cable type has skew worth
    having: a CAT5 balun, where the pairs are deliberately twisted at different
    rates and are therefore genuinely different lengths.
*/
const char* const kLineFragment = R"(#version 410 core
in vec2 uv;
out vec4 fragColor;

uniform sampler2D SourceTexture;
uniform vec2 MaxUV;
uniform vec2 OutputSize;

uniform float Kernel[ 64 ];
uniform int TapCount;
uniform vec3 SkewPx;
uniform int Split;

vec4 fetch( float px, float y )
{
	float x = clamp( px / OutputSize.x, 0.0, 1.0 );
	return texture( SourceTexture, vec2( x, y ) * MaxUV );
}

void main()
{
	float px = uv.x * OutputSize.x;

	vec4 sum = vec4( 0.0 );

	if( Split == 0 )
	{
		for( int n = 0; n < TapCount; ++n )
			sum += Kernel[ n ] * fetch( px - float( n ), uv.y );
	}
	else
	{
		for( int n = 0; n < TapCount; ++n )
		{
			float w = Kernel[ n ];
			float base = px - float( n );
			sum.r += w * fetch( base + SkewPx.r, uv.y ).r;
			sum.g += w * fetch( base + SkewPx.g, uv.y ).g;
			sum.b += w * fetch( base + SkewPx.b, uv.y ).b;

			//Alpha rides with the green conductor rather than getting a skew
			//of its own. There is no alpha on a cable at all; it is here so
			//the premultiplied colour and its coverage stay consistent, and
			//giving coverage a fourth arrival time would separate it from all
			//three of the signals it is meant to bound.
			sum.a += w * fetch( base + SkewPx.g, uv.y ).a;
		}
	}

	fragColor = vec4( sum.rgb, max( sum.a, 0.0 ) );
}
)";

/**
    An 8:1 horizontal reduction. Run twice, so the second one is 64:1.

    Horizontal only, and that is the point. The cable's response is a fact
    about TIME, a scan line is the only axis that carries time, and a
    reduction that touched the vertical axis would be inventing a coupling
    between scan lines that no cable has.
*/
const char* const kWideFragment = R"(#version 410 core
in vec2 uv;
out vec4 fragColor;

uniform sampler2D SourceTexture;
uniform float OutTexel;

void main()
{
	vec4 sum = vec4( 0.0 );
	float left = uv.x - 0.5 * OutTexel;

	for( int i = 0; i < 8; ++i )
	{
		float t = ( float( i ) + 0.5 ) / 8.0;
		sum += texture( SourceTexture, vec2( clamp( left + t * OutTexel, 0.0, 1.0 ), uv.y ) );
	}

	fragColor = sum * 0.125;
}
)";
} // namespace fivewire::shaders
