#include "../Shaders.h"

namespace fivewire::shaders
{
/**
    The far end of the cable.

    Four things arrive here that did not leave the amplifier, and every one of
    them is added AFTER the loss, because that is where they physically join
    the signal. Which is the whole argument of the plugin: the equaliser in the
    next pass has to lift all of this along with the picture, and the identical
    filter back at the amplifier does not.

      the tail       the rest of the cable's own response, hundreds of pixels
                     long, read out of the two reduced buffers. This is what
                     streaking is made of.
      reflections    the signal arriving again, later, softer, because both
                     ends are the wrong impedance. A ghost needs two
                     mismatches: the far end to send it back, and the
                     amplifier to send it forward again.
      crosstalk      the neighbouring conductor's RATE OF CHANGE, not its
                     level -- mutual inductance couples di/dt. So it appears
                     as coloured outlines on edges and never as a tint over
                     flat colour, which is the difference between crosstalk
                     and a bad white balance.
      pickup         mains hum and radio, in through whatever the shield let
                     past.

    The receiver's own timing lives here too, because sync came down the same
    cable. Line jitter and a rolling frame are not effects with sliders; they
    are what is left when the sync margin runs out.
*/
const char* const kComposeFragment = R"(#version 410 core
in vec2 uv;
out vec4 fragColor;

uniform sampler2D LineTexture;
uniform sampler2D Wide8Texture;
uniform sampler2D Wide64Texture;
uniform vec2 OutputSize;

uniform float Wide8W[ 8 ];
uniform float Wide64W[ 8 ];
uniform int UseWide;

uniform float GhostAmp[ 4 ];
uniform float GhostOffset[ 4 ];
uniform float GhostBlur[ 4 ];
uniform int GhostCount;

uniform float Crosstalk;
uniform float Noise;
uniform float Hum;
uniform float HumPerFrame;
uniform float HumPhase;
uniform float Ingress;
uniform float IngressPitch;
uniform float IngressPhase;

uniform float SyncDrive;
uniform float SyncLoss;
uniform float SogAmount;
uniform float Jitter;
uniform float JitterSeed;
uniform float RollOffset;

const float kTwoPi = 6.28318530718;

float hash( vec2 p )
{
	return fract( sin( dot( p, vec2( 127.1, 311.7 ) ) ) * 43758.5453123 );
}

vec4 tapLine( float px, float y )
{
	return texture( LineTexture, vec2( clamp( px / OutputSize.x, 0.0, 1.0 ), y ) );
}

vec4 tapWide( sampler2D source, float px, float y )
{
	return texture( source, vec2( clamp( px / OutputSize.x, 0.0, 1.0 ), y ) );
}

void main()
{
	float px = uv.x * OutputSize.x;

	//The receiver's vertical lock. Note what does NOT move with it: the hum
	//bar and the interference are generated at the un-rolled position, so a
	//picture that has lost lock runs upwards through a bar that stays where
	//it is. That is what a set with the vertical hold turned down looks like,
	//and rolling the interference along with the picture is the giveaway that
	//an effect has drawn a roll rather than lost lock.
	float y           = fract( uv.y + RollOffset );
	float lineFromTop = 1.0 - uv.y;
	float lineIndex   = floor( lineFromTop * OutputSize.y );

	//Mains phase at this line. The frame period is measured, not assumed, so
	//50 Hz on a 50 Hz frame rate really does stand still and one hertz out
	//really does crawl.
	float humWave = sin( HumPhase + HumPerFrame * lineFromTop );

	//What the receiver has left to find sync with. The green conductor's own
	//level enters here when sync rides on it: a bright green line loads the
	//sync tip, and the picture wobbles on exactly the shots that are green.
	float green  = texture( Wide64Texture, vec2( 0.5, y ) ).g;
	float margin = SyncDrive - SyncLoss - SogAmount * green - 0.15 * Hum * humWave;

	//What the line's timing costs. The constant is the receiver's own PLL,
	//which is never perfect; the squared term is the sync margin, squared
	//because a slicer copes perfectly well until it suddenly does not.
	float deficit  = clamp( 1.0 - margin, 0.0, 1.0 );
	float wander   = 0.15 + deficit * deficit;
	float jitterPx = Jitter * wander * ( hash( vec2( lineIndex, JitterSeed ) ) - 0.5 ) * 2.0;

	//Hum is on the sync conductor as well as the picture, which is why a hum
	//bar bends the lines it passes through instead of only dimming them.
	jitterPx += Hum * humWave * 2.0;

	float sx = px - jitterPx;

	vec4 acc = tapLine( sx, y );

	//---------------------------------------------------------------------
	// The rest of the cable's own response.
	//---------------------------------------------------------------------
	if( UseWide == 1 )
	{
		for( int j = 0; j < 8; ++j )
			acc += Wide8W[ j ] * tapWide( Wide8Texture, sx - ( 64.0 + 8.0 * float( j ) + 4.0 ), y );

		for( int j = 0; j < 8; ++j )
			acc += Wide64W[ j ] * tapWide( Wide64Texture, sx - ( 128.0 + 64.0 * float( j ) + 32.0 ), y );
	}

	//---------------------------------------------------------------------
	// Reflections. Softer every bounce, because every bounce is two more
	// trips down the same cable.
	//---------------------------------------------------------------------
	for( int i = 0; i < GhostCount; ++i )
	{
		vec4 g = vec4( 0.0 );
		for( int k = -2; k <= 2; ++k )
			g += tapLine( sx - GhostOffset[ i ] + float( k ) * GhostBlur[ i ] * 0.5, y );

		acc += GhostAmp[ i ] * g * 0.2;
	}

	//---------------------------------------------------------------------
	// Conductor to conductor.
	//---------------------------------------------------------------------
	if( Crosstalk > 0.0 )
	{
		vec3 slope = ( tapLine( sx + 1.0, y ).rgb - tapLine( sx - 1.0, y ).rgb ) * 0.5;
		acc.rgb += Crosstalk * vec3( slope.g + slope.b, slope.r + slope.b, slope.r + slope.g );
	}

	//---------------------------------------------------------------------
	// What the room put on top.
	//
	// All three are scaled by alpha. A cable has no alpha channel and this is
	// not physics -- it is the compositing rule: a premultiplied pixel may not
	// carry colour outside its own coverage, and noise glowing in the empty
	// half of a keyed layer is the result of forgetting it.
	//---------------------------------------------------------------------
	float a = max( acc.a, 0.0 );

	if( Noise > 0.0 )
	{
		vec2 seed = vec2( px + JitterSeed * 13.0, lineIndex * 3.0 + JitterSeed );
		vec3 n    = vec3( hash( seed + 1.0 ), hash( seed + 2.0 ), hash( seed + 3.0 ) ) - 0.5;
		acc.rgb += Noise * n * 2.0 * a;
	}

	acc.rgb += Hum * humWave * a;

	if( Ingress > 0.0 )
	{
		//The per-line phase advance is what turns a carrier into a
		//herringbone: the interferer has no idea the picture has lines, so
		//each line catches it at a different phase and the pattern leans.
		float carrier = sin( kTwoPi * IngressPitch * px + IngressPhase + lineIndex * 1.7 );
		acc.rgb += Ingress * carrier * a;
	}

	fragColor = vec4( acc.rgb, a );
}
)";
} // namespace fivewire::shaders
