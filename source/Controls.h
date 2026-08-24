#pragma once

#include "Cable.h"

/**
    Host parameters in, physical quantities out.

    Every control a host shows is 0..1, because `CFFGLPluginManager::SetParamInfo`
    clamps a `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange` gets a
    chance to widen it, and there is no `SetParamDefault` to work round it (SDK
    b1afaf9). So a control declared in metres cannot declare a default in
    metres, and the fleet's answer is to keep every slider 0..1 and do the
    mapping here.

    Which makes this the one file that says what 5-wire means by any of its
    numbers -- how long "Length" is, how fast "Pixel Clock" is, and which ohms
    "Termination" stands for -- and the only place a second build of the same
    effect could disagree with the first.
*/

namespace fivewire::controls
{
/// The most reflections drawn. Four is not a budget: past the fourth bounce
/// the round trip has been through the cable nine times, and on any run long
/// enough to make the ghost visible in the first place there is nothing left
/// of it.
inline constexpr int kMaxGhosts = 4;

//---------------------------------------------------------------------------
// The mappings. Each is the physical range one slider covers, and each is a
// function so the harness can print the number the operator is really setting.
//---------------------------------------------------------------------------

/// Metres of cable. Squared, so the bottom of the slider has the resolution:
/// the difference between 2 m and 10 m is nothing, and the difference between
/// 90 m and 100 m is whether the show works.
float Metres( float p );

/// Pixel clock in hertz. Logarithmic over the range a VGA input actually
/// accepts, 25 MHz (640x480) to 340 MHz (2560x1600 reduced blanking). The
/// default sits at 108 MHz, which is 1280x1024 at 60 Hz -- the mode most
/// long VGA runs in the field are carrying.
float PixelClockHz( float p );

/// The load at the far end, in ohms. Logarithmic about 75, so 0.5 is a
/// correctly terminated line and the two halves of the slider are the two
/// ways of getting it wrong: under-terminated below (someone left a
/// terminator on a through connection), open above (a monitor whose input is
/// high impedance and nothing terminating the end of the run).
float TerminationOhms( float p );

/// Reflection coefficient looking back into the amplifier -- what the Ghosting
/// slider really sets.
///
/// A ghost needs TWO mismatches, not one. The far end sends some of the signal
/// back; the amplifier at the near end has to send it forward again, and an
/// output stage with a proper series resistor simply absorbs it. So zero here
/// is a correctly back-matched amplifier and there is no ghost at any
/// termination, which is why a slider called Ghosting can be honest: its zero
/// is a real piece of hardware behaving properly rather than an effect being
/// switched off.
float SourceReflection( float p );

/// Mains frequency, in hertz. An option rather than a slider, because there
/// are two answers and picking between them decides whether the hum bar sits
/// still or crawls.
float MainsHz( int option );

/// Cycles per pixel of the interfering carrier. Below 0.5 or the herringbone
/// aliases into a coarse beat that says more about the sampling than about
/// the interference.
float IngressPitch( float p );

//---------------------------------------------------------------------------
// Everything the operator can reach, in host units.
//---------------------------------------------------------------------------
struct Settings
{
	//Cable
	int cableType      = 0;
	float length       = 0.5f;
	float pixelClock   = 0.56f;
	float termination  = 0.5f;
	float ghosting     = 0.3f;
	int bounces        = 1;
	float skew         = 0.35f;
	float crosstalk    = 0.35f;
	float screening    = 0.5f;

	//Interference
	int mains          = 0;
	float noise        = 0.2f;
	float hum          = 0.25f;
	float ingress      = 0.15f;
	float ingressPitch = 0.35f;

	//Sync
	float syncLevel    = 0.7f;
	bool syncOnGreen   = false;
	float jitter       = 0.3f;

	//Amplifier at the head of the run
	float gain         = 0.5f;
	float red          = 0.5f;
	float green        = 0.5f;
	float blue         = 0.5f;
	float preEmphasis  = 0.0f;
	float eqLength     = 0.5f;
	float headroom     = 0.5f;

	//Receiver
	float cableEq      = 0.0f;
	float outputGain   = 0.5f;
	float black        = 0.5f;
	float restore      = 0.85f;
	float samplePhase  = 0.0f;
};

/// What the shaders are actually given. Nothing in here is a host value and
/// nothing in here is a taste constant: every field is a physical quantity or
/// a filter tap computed from one.
struct Drive
{
	/// The amplifier's pre-emphasis, on its own.
	///
	/// It would be cheaper to convolve this into the cable's response and run
	/// one filter -- two linear filters in series are one filter -- but the
	/// amplifier has a RAIL, and a rail is not linear. Pre-emphasis overshoots
	/// on every edge, the overshoot is what hits the rail, and it has to hit
	/// it before the cable smears it rather than after. So they stay two
	/// passes, and `headroom` means what it says.
	float headKernel[ kTaps ] = {};
	int headTaps              = 1;

	/// False when the amplifier is at unity with no pre-emphasis, which is the
	/// default. The whole pass is then skipped and the cable reads the host's
	/// texture directly.
	bool useHead = false;

	/// The cable's own response.
	float cableKernel[ kTaps ] = {};

	/// How many taps of it are worth fetching. A short run puts everything in
	/// the first three or four, and the pass costs what the cable costs.
	int cableTaps = 1;

	/// True when the three conductors are different lengths, which triples the
	/// fetches in the cable pass. False for coax, where they are not.
	bool splitConductors = false;

	/// The same response's tail, as box weights over the reduced buffers.
	float wide[ kWideLevels ][ kWideTaps ] = {};

	/// False when the head holds essentially all of the response, which is
	/// every short run. Two reduction passes are skipped entirely.
	bool useWide = false;

	/// The receiver's equaliser, applied after the noise and the ghosts --
	/// which is exactly why it lifts them.
	float eqKernel[ kTaps ] = {};
	int eqTaps              = 1;

	/// Per-conductor arrival time, in pixels, relative to the picture. Sums to
	/// zero: skew separates the channels, it does not move the frame.
	float skewPx[ 3 ] = {};

	/// Master gain times the per-channel drive, at the head.
	float headGain[ 3 ] = { 1.0f, 1.0f, 1.0f };

	/// Where the output stage runs out of rail. Pre-emphasis overshoots on
	/// every edge, and this is what the overshoot hits.
	float headClip = 1.5f;

	//--- reflections ---
	float ghostAmp[ kMaxGhosts ]      = {};
	float ghostOffsetPx[ kMaxGhosts ] = {};
	/// Radius, in pixels, of the extra softening each bounce has picked up.
	///
	/// A ghost has been down the cable two more times than the picture for
	/// every bounce it has made, so it is ALWAYS softer than the picture and
	/// gets softer the further out it lands. A ghost as sharp as the picture
	/// is the clearest sign that an effect drew it rather than derived it.
	/// Taken from where the extra response reaches half height, which is
	/// 1.1 * alpha^2 for a cable -- an approximation of a cascade of the real
	/// kernel by a box of the same width, and the one deliberate shortcut in
	/// the model.
	float ghostBlurPx[ kMaxGhosts ]   = {};
	int ghostCount                    = 0;

	//--- what the run picks up ---
	float crosstalk    = 0.0f;
	float noise        = 0.0f;
	float hum          = 0.0f;
	/// Radians of mains phase across one frame, and where that phase starts.
	/// Together these decide whether the hum bar stands still or crawls, and
	/// nothing here assumes 50 or 60 of anything.
	float humPerFrame  = 0.0f;
	float humPhase     = 0.0f;
	float ingress      = 0.0f;
	float ingressPitch = 0.0f;
	float ingressPhase = 0.0f;

	//--- sync ---
	float syncDrive  = 1.0f;
	/// How much of the sync amplitude the cable has taken, before any picture
	/// content is considered.
	float syncLoss   = 0.0f;
	/// Nonzero when sync rides on the green conductor, in which case a bright
	/// green line loads the sync and the receiver's timing suffers for it.
	float sogAmount  = 0.0f;
	float jitter     = 0.0f;
	float jitterSeed = 0.0f;
	/// Vertical offset when the receiver has lost vertical lock, 0..1.
	float rollOffset = 0.0f;

	//--- receiver ---
	float outGain     = 1.0f;
	float black       = 0.0f;
	float restore     = 1.0f;
	float samplePhase = 0.0f;

	//--- reported, never rendered from ---
	float alpha        = 0.0f;
	float metres       = 0.0f;
	float transitPx    = 0.0f;
	float bandwidth    = 1.0f;
};

/// The whole mapping, in one call, shared by every build.
///
/// `time` is seconds and `framePeriod` is how long the host is taking over one
/// frame -- measured, not assumed, because it is what turns a mains frequency
/// into a hum bar that either sits still or crawls.
Drive drive( const Settings& settings, float time, float framePeriod, float outputWidth );

} // namespace fivewire::controls
