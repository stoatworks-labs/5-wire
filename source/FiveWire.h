#pragma once

#include <FFGLSDK.h>

#include <chrono>
#include <string>

#include "Controls.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    5-wire -- a long run of RGBHV, and the amplifier at each end of it.

    Red, green, blue, H and V: five conductors, and everything this plugin
    renders is what happens to a signal on its way down them. There is one
    idea and the rest is consequence.

    **The cable is a filter, and the filter is not symmetric.** Coax loses the
    square root of frequency, and a square-root loss has a closed-form step
    response -- erfc( alpha / 2*sqrt(t) ) -- that is strictly causal with a
    tail hundreds of pixels long. So a bright edge smears to the RIGHT, softly
    at first and then for a very long way, and that single curve is the soft
    picture AND the streaking behind every caption. A symmetric blur is the
    commonest way to get this wrong and it reads instantly as "blurred" rather
    than as "long cable".

    Everything else follows from where things sit in the chain rather than from
    a control of its own:

      the ghost         both ends are the wrong impedance, so the signal
                        arrives again at twice the transit time -- and softer,
                        because it has been down the cable twice more
      colour fringes    the three conductors are different lengths
      coloured outlines the conductors couple each other's RATE OF CHANGE
      hum and hash      whatever the shield let in, over a longer aerial
      jitter and roll   sync came down the same cable, and the receiver has
                        run out of margin

    And the one that is the reason the plugin exists: **the equaliser at the
    receiving end lifts the noise and the ghosts with the picture, and the
    identical filter at the sending end does not**, because they sit on
    opposite sides of the place where the noise joins. That is why a real
    distribution amplifier offers both, and it is why Pre-Emphasis and Cable EQ
    are two controls here rather than one.

    See Cable.h for the model, Shaders.h for the passes, and AGENTS.md for the
    traps.
*/
class FiveWire : public CFFGLPlugin
{
public:
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous.
	void SetClockScaleForTest( double scale );

	/// What the harness compares the GPU's uniforms against.
	fivewire::controls::Drive DriveForTest( float time, float framePeriod, float width ) const;

	FiveWire();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Display-only text still has to accept a write.
	///
	/// `instantiateGL` sets EVERY parameter's default on a fresh instance and
	/// deletes the instance if any set returns FF_FAIL (SDK b1afaf9, FFGL.cpp
	/// ~289), and the base class's SetTextParameter is a stub that returns
	/// FF_FAIL. So a plugin that declares the About text block without
	/// overriding this cannot be instantiated by any real host at all -- while
	/// remaining perfectly happy in every harness in this repo, because they
	/// drive the plugin class directly and never go through plugMain.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;

	/// For the preset test, which drives the class directly.
	static const unsigned int* PresetParamIDsForTest( int& count );

private:
	/// Everything the operator can reach, in the order the signal meets it:
	/// the cable, what the room does to it, whether the receiver can find
	/// sync, the amplifier that drove it, and the one that receives it.
	enum ParamID : FFUInt32
	{
		//Cable
		PT_CABLE_TYPE,
		PT_LENGTH,
		PT_PIXEL_CLOCK,
		PT_TERMINATION,
		PT_GHOSTING,
		PT_BOUNCES,
		PT_SKEW,
		PT_CROSSTALK,
		PT_SCREENING,

		//Interference
		PT_NOISE,
		PT_HUM,
		PT_MAINS,
		PT_INGRESS,
		PT_INGRESS_PITCH,

		//Sync
		PT_SYNC_LEVEL,
		PT_SYNC_ON_GREEN,
		PT_JITTER,

		//Amplifier
		PT_GAIN,
		PT_RED,
		PT_GREEN,
		PT_BLUE,
		PT_PRE_EMPHASIS,
		PT_EQ_LENGTH,
		PT_HEADROOM,

		//Receiver
		PT_CABLE_EQ,
		PT_OUTPUT_GAIN,
		PT_BLACK,
		PT_RESTORE,
		PT_SAMPLE_PHASE,

		//Preset. Declared after the real controls so their IDs -- which a
		//saved composition refers to -- do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ fivewire::presets::kParamCount ] = {
		PT_CABLE_TYPE, PT_LENGTH, PT_TERMINATION, PT_GHOSTING, PT_BOUNCES,
		PT_SKEW, PT_CROSSTALK, PT_SCREENING,
		PT_NOISE, PT_HUM, PT_INGRESS, PT_INGRESS_PITCH,
		PT_SYNC_LEVEL, PT_SYNC_ON_GREEN, PT_JITTER,
		PT_GAIN, PT_RED, PT_GREEN, PT_BLUE, PT_PRE_EMPHASIS, PT_EQ_LENGTH, PT_HEADROOM,
		PT_CABLE_EQ, PT_OUTPUT_GAIN, PT_BLACK, PT_RESTORE, PT_SAMPLE_PHASE
	};

	void applyPreset( int presetIndex );
	float presetValue( int presetIndex, unsigned int id ) const;

	/// See reference_plugin_factory_presets: Resolume does NOT consume
	/// FF_EVENT_FLAG_VALUE. It keeps pushing the values it still believes in,
	/// those arrive here as ordinary parameter writes carrying a changed
	/// value, and a preset applied by copying is undone by the host's own echo
	/// before the next frame. These two are what tell an operator's edit from
	/// the host restating itself.
	void seedHostValues();
	bool hostIsRestatingItself( unsigned int index, float value );

	fivewire::controls::Settings settingsFromParams() const;

	bool compileShaders();
	void releaseBuffers();

	/// The host's clock, in seconds, whatever unit it arrived in.
	///
	/// Resolume sends `SetTime` in MILLISECONDS -- measured live in Arena
	/// 7.27.1 at 20.0 per frame at 50 fps. The FFGL header never says, this
	/// repo's own harness sends seconds, and the SDK's Particles sample
	/// quietly divides by a thousand. Here it decides how fast a hum bar
	/// crawls and how fast an unlocked frame rolls, so getting it wrong is
	/// three orders of magnitude of wrong.
	double nowSeconds();

	ffglex::FFGLShader headShader;
	ffglex::FFGLShader lineShader;
	ffglex::FFGLShader wideShader;
	ffglex::FFGLShader composeShader;
	ffglex::FFGLShader receiveShader;
	ffglex::FFGLScreenQuad quad;

	fivewire::PassBuffer headBuffer;
	fivewire::PassBuffer lineBuffer;
	fivewire::PassBuffer wide8Buffer;
	fivewire::PassBuffer wide64Buffer;
	fivewire::PassBuffer composeBuffer;

	//--- the clock ---------------------------------------------------------
	bool hostTimeSeen = false;
	std::chrono::steady_clock::time_point startTime;
	double clockScale   = 0.0;//!< 0 = undecided, 1 = seconds, 0.001 = milliseconds
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	/// How long the host is taking over one frame, measured rather than
	/// assumed. It is what turns a mains frequency into a hum bar that either
	/// stands still or crawls, and assuming 60 makes that behaviour simply
	/// wrong at every other rate.
	double framePeriod = 1.0 / 60.0;

	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ PT_COUNT ] = {};

	/// What the host last SENT, kept apart from what the plugin renders with.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
