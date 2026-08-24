#include "../Shaders.h"

namespace fivewire::shaders
{
/**
    The receiving end: equaliser, clamp, sampler.

    Three stages, in the order the hardware has them, and the order is the
    point.

    **The equaliser is the inverse of the cable, and it is applied here.** It
    is the same filter the amplifier could have applied at the other end, and
    it does the same thing to the picture. What is different is everything
    that joined the signal in between: the noise, the reflections and the
    crosstalk are all in the picture by the time this runs, so lifting the top
    end lifts them too. That is why a long run equalised at the receiver looks
    grainy and a long run pre-emphasised at the source does not -- and why a
    real distribution amplifier offers both.

    **The clamp restores black, and when it fails the black level follows the
    picture.** A video signal is AC coupled; without a working clamp its
    average is forced to zero, so a bright area pushes everything after it
    DOWN. That is streaking, it is why an overexposed caption leaves a dark
    trail across the rest of the line, and here it is the running average of
    the preceding few hundred pixels, subtracted.

    **The sampler has a phase.** The ADC in a display samples once per pixel,
    and where in the pixel it samples is the "phase" button on every monitor
    ever made. At the right phase it samples where the signal has settled; half
    a pixel out it samples the transition, and fine vertical detail loses
    contrast and shimmers. There is nothing extra in the shader for that -- it
    is a fractional offset and a bilinear fetch, which is precisely what the
    error is.
*/
const char* const kReceiveFragment = R"(#version 410 core
in vec2 uv;
out vec4 fragColor;

uniform sampler2D ComposeTexture;
uniform sampler2D Wide64Texture;
uniform vec2 OutputSize;

uniform float Kernel[ 64 ];
uniform int TapCount;

uniform float SamplePhase;
uniform float Restore;
uniform float Black;
uniform float OutGain;

vec4 fetch( float px, float y )
{
	return texture( ComposeTexture, vec2( clamp( px / OutputSize.x, 0.0, 1.0 ), y ) );
}

void main()
{
	//The sampling instant, in pixels. Zero samples where the signal has
	//settled; a half samples the transition.
	float px = uv.x * OutputSize.x + SamplePhase;

	vec4 sum = vec4( 0.0 );
	for( int n = 0; n < TapCount; ++n )
		sum += Kernel[ n ] * fetch( px - float( n ), uv.y );

	float a = max( sum.a, 0.0 );

	//The clamp. 256 pixels back is the centre of the window the second
	//reduction covers, which stands in for the restoration time constant.
	if( Restore < 1.0 )
	{
		vec3 average = texture( Wide64Texture,
		                        vec2( clamp( ( px - 256.0 ) / OutputSize.x, 0.0, 1.0 ), uv.y ) ).rgb;
		sum.rgb -= ( 1.0 - Restore ) * 0.8 * average;
	}

	sum.rgb = sum.rgb * OutGain + Black * a;

	//The display clips, and it clips at coverage rather than at one: this is
	//premultiplied, so a pixel that is half transparent runs out of white at
	//a half.
	float outA = clamp( a, 0.0, 1.0 );
	fragColor  = vec4( clamp( sum.rgb, vec3( 0.0 ), vec3( outA ) ), outA );
}
)";
} // namespace fivewire::shaders
