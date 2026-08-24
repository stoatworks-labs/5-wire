#include "../Shaders.h"

namespace fivewire::shaders
{
/**
    The amplifier at the head of the run.

    Gain, the three channel drives, pre-emphasis, and the rail.

    This is a separate pass from the cable purely because of that last one. Two
    linear filters in series are one filter, and folding pre-emphasis into the
    cable's response would save a full-resolution pass -- but an amplifier's
    supply rail is not linear, pre-emphasis overshoots on every edge by
    design, and the overshoot has to meet the rail BEFORE the cable smears it,
    not after. Get that order wrong and Headroom becomes a control that does
    almost nothing, because by then the peak it was supposed to catch has been
    spread over sixty pixels.

    Skipped entirely when the amplifier is at unity with no pre-emphasis, which
    is the default: the cable pass then reads the host's texture directly.
*/
const char* const kHeadFragment = R"(#version 410 core
in vec2 uv;
out vec4 fragColor;

uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 OutputSize;

uniform float Kernel[ 64 ];
uniform int TapCount;
uniform vec3 HeadGain;
uniform float HeadClip;

vec4 fetch( float px, float y )
{
	float x = clamp( px / OutputSize.x, 0.0, 1.0 );
	return texture( InputTexture, vec2( x, y ) * MaxUV );
}

void main()
{
	float px = uv.x * OutputSize.x;

	vec4 sum = vec4( 0.0 );
	for( int n = 0; n < TapCount; ++n )
		sum += Kernel[ n ] * fetch( px - float( n ), uv.y );

	float a = max( sum.a, 0.0 );
	vec3 rgb = sum.rgb * HeadGain;

	//The rail. Positive is the supply and it clips hard, which is what a
	//white with too much pre-emphasis on it actually does. The small negative
	//limit is the other rail: an overshoot below black is a real signal that
	//survives the whole cable and is thrown away by the display at the far
	//end, so it must NOT be clamped to zero here.
	fragColor = vec4( clamp( rgb, -0.3 * a, HeadClip * a ), a );
}
)";
} // namespace fivewire::shaders
