#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace fivewire::controls
{
namespace
{
float lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

constexpr float kTwoPi = 6.28318530717958647692f;

/// Where the cable's own response reaches half its height, in pixels, for a
/// given loss constant. erfc(a/(2*sqrt(t))) = 0.5 at a/(2*sqrt(t)) = 0.4769.
float halfHeightPixels( float alpha )
{
	const float root = alpha / 0.95387f;
	return root * root;
}

/// How many taps of a kernel are worth fetching.
///
/// This is a real economy rather than a micro-optimisation: at 5 m of decent
/// coax the response is one tap and a rounding error, and a loop that runs 64
/// times per pixel anyway would make the cheapest possible setting of this
/// plugin the most expensive thing in the composition.
int significantTaps( const float taps[ kTaps ] )
{
	int last = 0;
	for( int n = 0; n < kTaps; ++n )
		if( std::fabs( taps[ n ] ) > 1.0e-4f )
			last = n;

	return last + 1;
}
} // namespace

//---------------------------------------------------------------------------
float Metres( float p )
{
	//Squared, so the useful resolution is at the bottom. Between 2 m and 10 m
	//there is nothing to see; between 90 m and 100 m is the difference
	//between a picture and an argument with the venue.
	const float t = std::clamp( p, 0.0f, 1.0f );
	return 150.0f * t * t;
}

float PixelClockHz( float p )
{
	//25 MHz is 640x480 and 340 MHz is about as fast as a VGA input is ever
	//asked to go. Logarithmic, because the interesting comparison is a
	//doubling and not an increment.
	const float t = std::clamp( p, 0.0f, 1.0f );
	return 25.0e6f * std::pow( 13.6f, t );
}

float TerminationOhms( float p )
{
	//Geometric about 75, so half the slider is each way of being wrong and
	//the middle really is right rather than nearly right.
	const float t = std::clamp( p, 0.0f, 1.0f );
	return 75.0f * std::pow( 4.0f, 2.0f * t - 1.0f );
}

float SourceReflection( float p )
{
	//Stops short of 1.0: an amplifier output is never a perfect open circuit,
	//and a coefficient of exactly one would put a ghost train down the whole
	//line with no loss of amplitude at all.
	return std::clamp( p, 0.0f, 1.0f ) * 0.9f;
}

float MainsHz( int option )
{
	return option == 1 ? 60.0f : 50.0f;
}

float IngressPitch( float p )
{
	//Stops below Nyquist. Past 0.5 cycles per pixel the herringbone aliases
	//into a coarse beat that is a fact about the sampling rather than about
	//the transmitter down the road.
	return lerp( 0.02f, 0.45f, std::clamp( p, 0.0f, 1.0f ) );
}

//---------------------------------------------------------------------------
Drive drive( const Settings& settings, float time, float framePeriod, float outputWidth )
{
	Drive d;

	const CableSpec& spec = cable( settings.cableType );

	const float metres = Metres( settings.length );
	const float clock  = PixelClockHz( settings.pixelClock );
	const float alpha  = alphaFor( spec, metres, clock );

	d.alpha     = alpha;
	d.metres    = metres;
	d.transitPx = transitPixels( spec, metres, clock );
	d.bandwidth = bandwidthCyclesPerPixel( alpha );

	//---------------------------------------------------------------------
	// The head of the run, and then the cable. Two filters, and they stay two
	// because the amplifier between them has a rail -- see Drive::headKernel.
	//---------------------------------------------------------------------
	const Kernel loss = lossKernel( alpha );

	//The equaliser is calibrated in METRES, exactly as the knob on a real
	//cable equaliser is, so it can be set for a different length than the one
	//actually on the floor -- which is the ordinary way of getting it wrong.
	const float eqAlpha = alphaFor( spec, Metres( settings.eqLength ), clock );

	equaliserKernel( eqAlpha, settings.preEmphasis * 1.5f, d.headKernel );
	d.headTaps = significantTaps( d.headKernel );

	for( int n = 0; n < kTaps; ++n )
		d.cableKernel[ n ] = loss.tap[ n ];
	d.cableTaps = significantTaps( d.cableKernel );

	for( int level = 0; level < kWideLevels; ++level )
		for( int j = 0; j < kWideTaps; ++j )
			d.wide[ level ][ j ] = loss.wide[ level ][ j ];

	//Below this the head holds effectively the whole response and the two
	//reduction passes are skipped -- which is every short run, i.e. most of
	//the time this plugin is switched on.
	d.useWide = loss.headSum < 0.995f;

	equaliserKernel( eqAlpha, settings.cableEq * 1.5f, d.eqKernel );
	d.eqTaps = significantTaps( d.eqKernel );

	//---------------------------------------------------------------------
	// Per-conductor arrival time.
	//---------------------------------------------------------------------
	const float skewSeconds = spec.skewNsPer100m * 1.0e-9f * ( metres / 100.0f );
	const float skewPixels  = skewSeconds * clock * settings.skew * 2.5f;
	for( int i = 0; i < 3; ++i )
		d.skewPx[ i ] = spec.skewPattern[ i ] * skewPixels;

	//Three conductors of the same length are three fetches that would land on
	//the same texel, so the cable pass has a cheap path and this is what
	//chooses it. A twentieth of a pixel is well under what a bilinear fetch
	//can express.
	d.splitConductors = std::fabs( d.skewPx[ 0 ] - d.skewPx[ 2 ] ) > 0.05f;

	//---------------------------------------------------------------------
	// The amplifier's own knobs.
	//---------------------------------------------------------------------
	const float master = settings.gain * 2.0f;
	d.headGain[ 0 ]    = master * settings.red * 2.0f;
	d.headGain[ 1 ]    = master * settings.green * 2.0f;
	d.headGain[ 2 ]    = master * settings.blue * 2.0f;
	d.headClip         = 1.0f + settings.headroom * 2.0f;

	//Skipped entirely at unity with no pre-emphasis, which is the default: the
	//cable pass then reads the host's own texture and the amplifier costs
	//nothing at all.
	d.useHead = settings.preEmphasis > 0.001f
	            || std::fabs( d.headGain[ 0 ] - 1.0f ) > 0.001f
	            || std::fabs( d.headGain[ 1 ] - 1.0f ) > 0.001f
	            || std::fabs( d.headGain[ 2 ] - 1.0f ) > 0.001f;

	//---------------------------------------------------------------------
	// Reflections.
	//
	// A ghost needs a mismatch at BOTH ends: the far end sends energy back,
	// and the near end has to send it forward again. The product is what
	// arrives, and it is why a properly back-matched amplifier kills ghosting
	// stone dead on a line nobody has terminated.
	//---------------------------------------------------------------------
	const float gammaLoad   = reflection( TerminationOhms( settings.termination ) );
	const float gammaSource = SourceReflection( settings.ghosting );
	const float product     = gammaLoad * gammaSource;

	d.ghostCount = 0;
	for( int n = 1; n <= std::clamp( settings.bounces, 1, kMaxGhosts ); ++n )
	{
		const float offset = 2.0f * static_cast< float >( n ) * d.transitPx;

		//A repeat further away than the picture is wide has left the screen.
		//Dropping it saves the taps rather than drawing something nobody can
		//see.
		if( offset > outputWidth )
			break;

		const float amplitude = std::pow( product, static_cast< float >( n ) );
		if( std::fabs( amplitude ) < 0.002f )
			break;

		const int i = d.ghostCount++;
		d.ghostAmp[ i ]      = amplitude;
		d.ghostOffsetPx[ i ] = offset;

		//Two extra transits per bounce, so the extra loss constant is 2n
		//times the cable's own.
		d.ghostBlurPx[ i ] = std::min( 64.0f, halfHeightPixels( 2.0f * static_cast< float >( n ) * alpha ) );
	}

	//---------------------------------------------------------------------
	// What the run picks up on the way.
	//
	// Screening and length together: a shield keeps the room out, and a
	// longer run is a longer aerial. Hum and ingress share both terms because
	// they arrive by the same route -- there is no cable that is good at
	// keeping mains out and bad at keeping radio out.
	//---------------------------------------------------------------------
	const float shielding    = std::clamp( spec.shielding * ( 0.5f + settings.screening ), 0.0f, 1.0f );
	const float lengthAerial = std::clamp( std::sqrt( metres / 25.0f ), 0.0f, 2.5f );
	const float pickup       = ( 1.0f - shielding ) * lengthAerial;

	//Crosstalk is the ONLY one of these that is not pickup from outside: it is
	//the conductors coupling into each other, so it scales with length and
	//with how tightly they are bundled, and it is a derivative -- see the
	//compose pass.
	d.crosstalk = std::clamp( spec.crosstalkPer100m * ( metres / 100.0f ) * settings.crosstalk * 1.5f,
	                          0.0f, 2.5f );

	//Thermal noise, which is the receiver's own and has nothing to do with the
	//shield. It is added at the END of the cable, which is what makes the
	//equaliser lift it and pre-emphasis not.
	d.noise = settings.noise * 0.12f;

	const float mains = MainsHz( settings.mains );
	d.hum             = settings.hum * 0.22f * pickup;
	d.humPerFrame     = kTwoPi * mains * framePeriod;
	d.humPhase        = std::fmod( kTwoPi * mains * time, kTwoPi );

	d.ingress      = settings.ingress * 0.18f * pickup;
	d.ingressPitch = IngressPitch( settings.ingressPitch );
	d.ingressPhase = time * 37.0f;

	//---------------------------------------------------------------------
	// Sync, on its own two conductors.
	//
	// The receiver has to recover timing from a signal that has been down the
	// same cable, and everything that goes wrong with the picture when it
	// cannot is a CONSEQUENCE here rather than a control: line jitter when
	// the margin is thin, and the frame rolling when it runs out. There is no
	// Roll slider on purpose.
	//---------------------------------------------------------------------
	d.syncDrive = settings.syncLevel * 2.0f;

	//Sync barely notices the cable, and getting that wrong is worth spelling
	//out. An H sync pulse is a few microseconds wide -- tens of kilohertz --
	//against a pixel clock of a hundred megahertz, so a run that has thrown
	//away three quarters of the picture's bandwidth has taken almost nothing
	//off the sync AMPLITUDE. What it has taken is the EDGE: the pulse arrives
	//with a slower rise, and the receiver's slicer crosses it at a less
	//certain moment. So length buys jitter, not loss of lock -- and loss of
	//lock, when it comes, comes from an amplifier that was not driving enough
	//sync in the first place.
	//
	//This was first written as `alpha * 0.9`, which put a hundred-metre run
	//into a rolling frame on its own. It looked spectacular and it is not what
	//happens: a long run goes soft and stays locked.
	d.syncLoss  = std::clamp( alpha * 0.18f, 0.0f, 0.6f );
	d.sogAmount = settings.syncOnGreen ? 0.5f : 0.0f;

	//The receiver's own timing, and then what the slow edge costs it. The
	//shader adds the sync-margin term on top of this; what is here is the part
	//that is about the cable rather than about the amplifier.
	d.jitter    = settings.jitter * 8.0f * ( 0.5f + 0.5f * std::clamp( alpha, 0.0f, 2.0f ) );

	//A seed that changes every frame, so jitter is a different set of lines
	//each time rather than a fixed pattern that reads as texture.
	d.jitterSeed = std::fmod( time * 60.0f, 4096.0f );

	const float margin = d.syncDrive - d.syncLoss;
	if( margin < 0.35f )
	{
		//Vertical lock goes last and goes gradually: the frame starts to
		//creep, then runs. Somebody who has ever turned the V-hold on a
		//television knows the shape of this.
		const float rollRate = ( 0.35f - margin ) * 2.5f;
		d.rollOffset         = std::fmod( time * rollRate, 1.0f );
	}

	//---------------------------------------------------------------------
	// The receiver.
	//---------------------------------------------------------------------
	d.outGain     = settings.outputGain * 2.0f;
	d.black       = ( settings.black - 0.5f ) * 0.4f;
	d.restore     = settings.restore;
	d.samplePhase = settings.samplePhase;

	return d;
}

} // namespace fivewire::controls
