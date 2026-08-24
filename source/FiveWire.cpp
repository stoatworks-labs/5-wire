#include "FiveWire.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace fivewire;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FiveWire >,                                        // Create method
	"5W01",                                                           // Plugin unique ID of maximum length 4.
	"5-wire",                                                         // Plugin name
	2,                                                                // API major version number
	1,                                                                // API minor version number
	1,                                                                // Plugin major version number
	0,                                                                // Plugin minor version number
	FF_EFFECT,                                                        // Plugin type
	"A long run of VGA or RGBHV, and the amplifier at each end of it",// Plugin description
	"5-wire FFGL effect"                                              // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

const char* const kMainsNames[]   = { "50 Hz", "60 Hz" };
const char* const kBounceNames[]  = { "1", "2", "3", "4" };

constexpr int kMainsCount  = 2;
constexpr int kBounceCount = 4;

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Upload a whole uniform array.
///
/// `FFGLShader::Set` has no array overload and never will -- it is built out
/// of glUniform1f/2f/3f/4f. A kernel is 64 floats and setting them one at a
/// time by name would be 64 glGetUniformLocation calls per pass per frame, so
/// this goes straight to the GL. A name that does not exist gives -1, and
/// glUniform1fv against -1 is a documented no-op: the pass renders with a
/// kernel of zeros and shows an entirely black picture, which is at least an
/// unmissable failure rather than a subtle one.
void setArray( const FFGLShader& shader, const char* name, const float* values, int count )
{
	const GLint location = glGetUniformLocation( shader.GetGLID(), name );
	if( location >= 0 )
		glUniform1fv( location, count, values );
}
} // namespace

//---------------------------------------------------------------------------
FiveWire::FiveWire() :
	startTime( std::chrono::steady_clock::now() )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Hum crawls, jitter re-rolls and an unlocked frame runs, so the effect
	//needs a clock. Asking the host for one means a re-render of the same
	//composition produces the same noise rather than whatever the wall clock
	//happened to say when it was rendered.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults.
	//
	// SetParamInfof reads each one back out of GetFloatParameter, so these
	// assignments are what the host is told the defaults are.
	//
	// They are a cheap moulded VGA lead about eighteen metres long, into a
	// display nobody terminated: soft, a ghost twenty pixels to the right, a
	// little hum and hash, and the receiver's equaliser doing some of the work
	// and lifting the noise while it does. That is the effect this plugin
	// exists for, and an effect that does nothing until six sliders have been
	// moved is an effect nobody finds out is any good.
	//---------------------------------------------------------------------
	params[ PT_CABLE_TYPE ]    = 1.0f; //VGA Lead
	params[ PT_LENGTH ]        = 0.35f;//about 18 m
	params[ PT_PIXEL_CLOCK ]   = 0.56f;//about 108 MHz -- 1280x1024 at 60
	params[ PT_TERMINATION ]   = 0.70f;//nothing terminating the far end
	params[ PT_GHOSTING ]      = 0.35f;
	params[ PT_BOUNCES ]       = 0.0f; //one repeat
	params[ PT_SKEW ]          = 0.35f;
	params[ PT_CROSSTALK ]     = 0.40f;
	params[ PT_SCREENING ]     = 0.50f;//as the cable type says it is

	params[ PT_NOISE ]         = 0.22f;
	params[ PT_HUM ]           = 0.35f;
	params[ PT_MAINS ]         = 0.0f; //50 Hz
	params[ PT_INGRESS ]       = 0.20f;
	params[ PT_INGRESS_PITCH ] = 0.35f;

	params[ PT_SYNC_LEVEL ]    = 0.75f;
	params[ PT_SYNC_ON_GREEN ] = 0.0f;
	params[ PT_JITTER ]        = 0.30f;

	params[ PT_GAIN ]          = 0.50f;//0.5 is unity
	params[ PT_RED ]           = 0.50f;
	params[ PT_GREEN ]         = 0.50f;
	params[ PT_BLUE ]          = 0.50f;
	params[ PT_PRE_EMPHASIS ]  = 0.0f;
	params[ PT_EQ_LENGTH ]     = 0.35f;//the equaliser set for the cable that is there
	params[ PT_HEADROOM ]      = 0.50f;

	params[ PT_CABLE_EQ ]      = 0.30f;
	params[ PT_OUTPUT_GAIN ]   = 0.50f;//0.5 is unity
	params[ PT_BLACK ]         = 0.50f;//0.5 is no offset
	params[ PT_RESTORE ]       = 0.80f;
	params[ PT_SAMPLE_PHASE ]  = 0.0f; //on the money

	params[ PT_PRESET ]        = 0.0f; //Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration. The groups are the signal path in order, which is also the
	// order somebody diagnosing a real run would work through it.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_CABLE_TYPE, "Cable Type", cableCount(), params[ PT_CABLE_TYPE ] );
	for( int i = 0; i < cableCount(); ++i )
		SetParamElementInfo( PT_CABLE_TYPE, i, cable( i ).name, static_cast< float >( i ) );

	SetParamInfof( PT_LENGTH, "Length", FF_TYPE_STANDARD );
	SetParamInfof( PT_PIXEL_CLOCK, "Pixel Clock", FF_TYPE_STANDARD );
	SetParamInfof( PT_TERMINATION, "Termination", FF_TYPE_STANDARD );
	SetParamInfof( PT_GHOSTING, "Ghosting", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BOUNCES, "Bounces", kBounceCount, params[ PT_BOUNCES ] );
	for( int i = 0; i < kBounceCount; ++i )
		SetParamElementInfo( PT_BOUNCES, i, kBounceNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_SKEW, "Skew", FF_TYPE_STANDARD );
	SetParamInfof( PT_CROSSTALK, "Crosstalk", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCREENING, "Screening", FF_TYPE_STANDARD );

	SetParamInfof( PT_NOISE, "Noise", FF_TYPE_STANDARD );
	SetParamInfof( PT_HUM, "Hum", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_MAINS, "Mains", kMainsCount, params[ PT_MAINS ] );
	for( int i = 0; i < kMainsCount; ++i )
		SetParamElementInfo( PT_MAINS, i, kMainsNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_INGRESS, "Ingress", FF_TYPE_STANDARD );
	SetParamInfof( PT_INGRESS_PITCH, "Ingress Pitch", FF_TYPE_STANDARD );

	SetParamInfof( PT_SYNC_LEVEL, "Sync Level", FF_TYPE_STANDARD );
	SetParamInfo( PT_SYNC_ON_GREEN, "Sync On Green", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_JITTER, "Jitter", FF_TYPE_STANDARD );

	SetParamInfof( PT_GAIN, "Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_RED, "Red", FF_TYPE_STANDARD );
	SetParamInfof( PT_GREEN, "Green", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLUE, "Blue", FF_TYPE_STANDARD );
	SetParamInfof( PT_PRE_EMPHASIS, "Pre-Emphasis", FF_TYPE_STANDARD );
	SetParamInfof( PT_EQ_LENGTH, "EQ Length", FF_TYPE_STANDARD );
	SetParamInfof( PT_HEADROOM, "Headroom", FF_TYPE_STANDARD );

	SetParamInfof( PT_CABLE_EQ, "Cable EQ", FF_TYPE_STANDARD );
	SetParamInfof( PT_OUTPUT_GAIN, "Output Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLACK, "Black Level", FF_TYPE_STANDARD );
	SetParamInfof( PT_RESTORE, "DC Restore", FF_TYPE_STANDARD );
	SetParamInfof( PT_SAMPLE_PHASE, "Sample Phase", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else overrides
	// the covered parameters. See applyPreset and hostIsRestatingItself.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = PT_CABLE_TYPE; i <= PT_SCREENING; ++i )
		SetParamGroup( i, "Cable" );
	for( FFUInt32 i = PT_NOISE; i <= PT_INGRESS_PITCH; ++i )
		SetParamGroup( i, "Interference" );
	for( FFUInt32 i = PT_SYNC_LEVEL; i <= PT_JITTER; ++i )
		SetParamGroup( i, "Sync" );
	for( FFUInt32 i = PT_GAIN; i <= PT_HEADROOM; ++i )
		SetParamGroup( i, "Amplifier" );
	for( FFUInt32 i = PT_CABLE_EQ; i <= PT_SAMPLE_PHASE; ++i )
		SetParamGroup( i, "Receiver" );

	SetParamGroup( PT_PRESET, "Preset" );
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created 5-wire effect" );

	diag::init();
}

//---------------------------------------------------------------------------
// The clock
//---------------------------------------------------------------------------
FFResult FiveWire::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

double FiveWire::nowSeconds()
{
	double raw;
	if( hostTimeSeen && hostTime >= 0.0 )
	{
		raw = hostTime;
	}
	else
	{
		//No host clock at all. The wall clock is already in seconds, so the
		//unit question does not arise and the scale must not be applied to it.
		const auto elapsed = std::chrono::steady_clock::now() - startTime;
		return std::chrono::duration< double >( elapsed ).count();
	}

	//Decide the unit by measuring the host's clock against a real one: the
	//ratio is ~1 for a seconds host and ~1000 for a milliseconds host, and
	//nothing plausible sits between.
	const double wallNow =
	    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow;
}

//---------------------------------------------------------------------------
controls::Settings FiveWire::settingsFromParams() const
{
	controls::Settings s;

	s.cableType    = static_cast< int >( std::lround( params[ PT_CABLE_TYPE ] ) );
	s.length       = params[ PT_LENGTH ];
	s.pixelClock   = params[ PT_PIXEL_CLOCK ];
	s.termination  = params[ PT_TERMINATION ];
	s.ghosting     = params[ PT_GHOSTING ];
	s.bounces      = static_cast< int >( std::lround( params[ PT_BOUNCES ] ) ) + 1;
	s.skew         = params[ PT_SKEW ];
	s.crosstalk    = params[ PT_CROSSTALK ];
	s.screening    = params[ PT_SCREENING ];

	s.mains        = static_cast< int >( std::lround( params[ PT_MAINS ] ) );
	s.noise        = params[ PT_NOISE ];
	s.hum          = params[ PT_HUM ];
	s.ingress      = params[ PT_INGRESS ];
	s.ingressPitch = params[ PT_INGRESS_PITCH ];

	s.syncLevel    = params[ PT_SYNC_LEVEL ];
	s.syncOnGreen  = params[ PT_SYNC_ON_GREEN ] >= 0.5f;
	s.jitter       = params[ PT_JITTER ];

	s.gain         = params[ PT_GAIN ];
	s.red          = params[ PT_RED ];
	s.green        = params[ PT_GREEN ];
	s.blue         = params[ PT_BLUE ];
	s.preEmphasis  = params[ PT_PRE_EMPHASIS ];
	s.eqLength     = params[ PT_EQ_LENGTH ];
	s.headroom     = params[ PT_HEADROOM ];

	s.cableEq      = params[ PT_CABLE_EQ ];
	s.outputGain   = params[ PT_OUTPUT_GAIN ];
	s.black        = params[ PT_BLACK ];
	s.restore      = params[ PT_RESTORE ];
	s.samplePhase  = params[ PT_SAMPLE_PHASE ];

	return s;
}

controls::Drive FiveWire::DriveForTest( float time, float period, float width ) const
{
	return controls::drive( settingsFromParams(), time, period, width );
}

//---------------------------------------------------------------------------
bool FiveWire::compileShaders()
{
	const struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} passes[] = {
		{ &headShader, shaders::kHeadFragment, "head" },
		{ &lineShader, shaders::kLineFragment, "line" },
		{ &wideShader, shaders::kWideFragment, "wide" },
		{ &composeShader, shaders::kComposeFragment, "compose" },
		{ &receiveShader, shaders::kReceiveFragment, "receive" },
	};

	for( const auto& pass : passes )
	{
		if( !pass.shader->Compile( shaders::kVertex, pass.fragment ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message
			//anywhere. This line is the only record of which pass it was.
			diag::error( std::string( "the " ) + pass.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "5-wire: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult FiveWire::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "5-wire: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	clockFrames = 0;

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult FiveWire::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin. Read before
	//anything of ours touches it -- ScopedFBOBinding restores the framebuffer
	//binding and NOT the viewport, so every pass's ResizeViewPort() leaks into
	//the next one and the final pass, which draws into the host's own
	//framebuffer, has nothing of its own to size itself from.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const int outputW = std::max( 1, static_cast< int >( hostViewport[ 2 ] ) );
	const int outputH = std::max( 1, static_cast< int >( hostViewport[ 3 ] ) );

	//---------------------------------------------------------------------
	// The clock. framePeriod is measured because it is what decides whether
	// the hum bar stands still or crawls.
	//---------------------------------------------------------------------
	const double now = nowSeconds();

	if( lastNow >= 0.0 && now > lastNow )
	{
		const double delta = std::clamp( now - lastNow, 1.0 / 240.0, 1.0 / 10.0 );
		//Smoothed, because one long frame should not make the bar jump.
		framePeriod += ( delta - framePeriod ) * 0.15;
	}
	lastNow = now;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now )
		            + " framePeriod=" + std::to_string( framePeriod ) );

	const controls::Drive drive = controls::drive( settingsFromParams(),
	                                               static_cast< float >( now ),
	                                               static_cast< float >( framePeriod ),
	                                               static_cast< float >( outputW ) );

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens BEFORE any texture is bound. FFGLFBO::Initialise
	// sizes its new colour texture inside a ScopedTextureBinding, and every
	// Scoped* binding in the SDK CLEARS to 0 on scope exit rather than
	// restoring -- so allocating a buffer silently unbinds whatever was on the
	// active unit. The symptom is the dangerous part: correct on every frame
	// except the one that allocates.
	//
	// 16-bit float rather than 8-bit throughout. Pre-emphasis overshoots well
	// past white before the cable brings it back, the tail of the response is
	// a term of a few thousandths that has to survive being added, and the
	// equaliser then multiplies the lot -- all of which an 8-bit intermediate
	// quantises into banding that looks exactly like a badly dithered gradient
	// rather than like a cable.
	//---------------------------------------------------------------------
	const int wide8W  = std::max( 1, ( outputW + 7 ) / 8 );
	const int wide64W = std::max( 1, ( wide8W + 7 ) / 8 );

	//The reduced buffers are wanted for three different reasons and any one of
	//them is enough: the tail of the cable's response, the clamp's running
	//average, and the green conductor's level when sync rides on it.
	const bool needWide = drive.useWide || drive.restore < 0.999f || drive.sogAmount > 0.0f;

	if( !lineBuffer.Ensure( outputW, outputH, GL_RGBA16F )
	    || !wide8Buffer.Ensure( wide8W, outputH, GL_RGBA16F )
	    || !wide64Buffer.Ensure( wide64W, outputH, GL_RGBA16F )
	    || !composeBuffer.Ensure( outputW, outputH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	if( drive.useHead && !headBuffer.Ensure( outputW, outputH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the amplifier buffer" );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	const float outW              = static_cast< float >( outputW );
	const float outH              = static_cast< float >( outputH );

	//---------------------------------------------------------------------
	// 1. The amplifier. Skipped at unity with no pre-emphasis, in which case
	//    the cable reads the host's texture directly.
	//---------------------------------------------------------------------
	GLuint cableSource = input.Handle;
	float cableMaxU    = maxCoords.s;
	float cableMaxV    = maxCoords.t;

	if( drive.useHead )
	{
		ScopedFBOBinding fbo( headBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		headBuffer.ResizeViewPort();
		ScopedShaderBinding shader( headShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		headShader.Set( "InputTexture", 0 );
		headShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		headShader.Set( "OutputSize", outW, outH );
		setArray( headShader, "Kernel", drive.headKernel, kTaps );
		glUniform1i( glGetUniformLocation( headShader.GetGLID(), "TapCount" ), drive.headTaps );
		headShader.Set( "HeadGain", drive.headGain[ 0 ], drive.headGain[ 1 ], drive.headGain[ 2 ] );
		headShader.Set( "HeadClip", drive.headClip );

		quad.Draw();

		cableSource = headBuffer.GetTextureInfo().Handle;
		cableMaxU   = 1.0f;
		cableMaxV   = 1.0f;
	}

	//---------------------------------------------------------------------
	// 2. The cable.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( lineBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		lineBuffer.ResizeViewPort();
		ScopedShaderBinding shader( lineShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( cableSource );

		lineShader.Set( "SourceTexture", 0 );
		lineShader.Set( "MaxUV", cableMaxU, cableMaxV );
		lineShader.Set( "OutputSize", outW, outH );
		setArray( lineShader, "Kernel", drive.cableKernel, kTaps );
		glUniform1i( glGetUniformLocation( lineShader.GetGLID(), "TapCount" ), drive.cableTaps );
		lineShader.Set( "SkewPx", drive.skewPx[ 0 ], drive.skewPx[ 1 ], drive.skewPx[ 2 ] );
		glUniform1i( glGetUniformLocation( lineShader.GetGLID(), "Split" ), drive.splitConductors ? 1 : 0 );

		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 3. The two horizontal reductions. Horizontal ONLY: the cable's response
	//    is a fact about time, a scan line is the only axis carrying time, and
	//    reducing vertically would invent a coupling between lines that no
	//    cable has.
	//---------------------------------------------------------------------
	if( needWide )
	{
		const struct
		{
			PassBuffer* target;
			GLuint source;
			int width;
		} reductions[] = {
			{ &wide8Buffer, lineBuffer.GetTextureInfo().Handle, wide8W },
			{ &wide64Buffer, wide8Buffer.GetTextureInfo().Handle, wide64W },
		};

		for( const auto& pass : reductions )
		{
			ScopedFBOBinding fbo( pass.target->GetGLID(), ScopedFBOBinding::RB_REVERT );
			pass.target->ResizeViewPort();
			ScopedShaderBinding shader( wideShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( pass.source );

			wideShader.Set( "SourceTexture", 0 );
			wideShader.Set( "OutTexel", 1.0f / static_cast< float >( pass.width ) );

			quad.Draw();
		}
	}

	//---------------------------------------------------------------------
	// 4. The far end of the cable.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( composeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		composeBuffer.ResizeViewPort();
		ScopedShaderBinding shader( composeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, lineBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, wide8Buffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, wide64Buffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		composeShader.Set( "LineTexture", 0 );
		composeShader.Set( "Wide8Texture", 1 );
		composeShader.Set( "Wide64Texture", 2 );
		composeShader.Set( "OutputSize", outW, outH );

		setArray( composeShader, "Wide8W", drive.wide[ 0 ], kWideTaps );
		setArray( composeShader, "Wide64W", drive.wide[ 1 ], kWideTaps );
		glUniform1i( glGetUniformLocation( composeShader.GetGLID(), "UseWide" ), drive.useWide ? 1 : 0 );

		setArray( composeShader, "GhostAmp", drive.ghostAmp, controls::kMaxGhosts );
		setArray( composeShader, "GhostOffset", drive.ghostOffsetPx, controls::kMaxGhosts );
		setArray( composeShader, "GhostBlur", drive.ghostBlurPx, controls::kMaxGhosts );
		glUniform1i( glGetUniformLocation( composeShader.GetGLID(), "GhostCount" ), drive.ghostCount );

		composeShader.Set( "Crosstalk", drive.crosstalk );
		composeShader.Set( "Noise", drive.noise );
		composeShader.Set( "Hum", drive.hum );
		composeShader.Set( "HumPerFrame", drive.humPerFrame );
		composeShader.Set( "HumPhase", drive.humPhase );
		composeShader.Set( "Ingress", drive.ingress );
		composeShader.Set( "IngressPitch", drive.ingressPitch );
		composeShader.Set( "IngressPhase", drive.ingressPhase );

		composeShader.Set( "SyncDrive", drive.syncDrive );
		composeShader.Set( "SyncLoss", drive.syncLoss );
		composeShader.Set( "SogAmount", drive.sogAmount );
		composeShader.Set( "Jitter", drive.jitter );
		composeShader.Set( "JitterSeed", drive.jitterSeed );
		composeShader.Set( "RollOffset", drive.rollOffset );

		quad.Draw();

		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
	}

	//---------------------------------------------------------------------
	// 5. The receiver, straight into whatever the host handed us.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( receiveShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, composeBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, wide64Buffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		receiveShader.Set( "ComposeTexture", 0 );
		receiveShader.Set( "Wide64Texture", 1 );
		receiveShader.Set( "OutputSize", outW, outH );

		setArray( receiveShader, "Kernel", drive.eqKernel, kTaps );
		glUniform1i( glGetUniformLocation( receiveShader.GetGLID(), "TapCount" ), drive.eqTaps );

		receiveShader.Set( "SamplePhase", drive.samplePhase );
		receiveShader.Set( "Restore", drive.restore );
		receiveShader.Set( "Black", drive.black );
		receiveShader.Set( "OutGain", drive.outGain );

		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void FiveWire::releaseBuffers()
{
	headBuffer.Destroy();
	lineBuffer.Destroy();
	wide8Buffer.Destroy();
	wide64Buffer.Destroy();
	composeBuffer.Destroy();
}

FFResult FiveWire::DeInitGL()
{
	headShader.FreeGLResources();
	lineShader.FreeGLResources();
	wideShader.FreeGLResources();
	composeShader.FreeGLResources();
	receiveShader.FreeGLResources();
	quad.Release();
	releaseBuffers();

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------
FFResult FiveWire::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	seedHostValues();

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// back to Custom -- which is what makes presets look like they cannot be
	// selected at all. See AGENTS.md.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	const float previous = params[ index ];
	params[ index ]      = value;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom.
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				// Logged, unlike an ordinary parameter change: this is a state
				// change an operator can be surprised by, it happens once
				// rather than per frame, and diagnosing the same bug in
				// vertigo needed a code read precisely because nothing said it
				// had happened.
				diag::info( "preset dropped to Custom: parameter "
				            + std::to_string( index ) + " moved to "
				            + std::to_string( value ) );
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

const unsigned int* FiveWire::PresetParamIDsForTest( int& count )
{
	count = presets::kParamCount;
	return kPresetParamIDs;
}

float FiveWire::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return -1.0f;

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

void FiveWire::seedHostValues()
{
	// Seeded on first parameter traffic rather than in the constructor, so the
	// whole mechanism stays in one place. It has to happen BEFORE applyPreset
	// can run: seeding afterwards would record the preset's own values as the
	// host's opening position, and the host's very next restatement would then
	// look like an edit -- which is the bug this exists to fix, reintroduced.
	if( hostValuesSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];
	hostValuesSeeded = true;
}

bool FiveWire::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;

	const float fromPreset =
	    presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a UI,
	// a MIDI value or a saved composition -- hands back a number near ours
	// rather than ours, and 1e-4 read that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
	{
		// The host agreeing with the preset. Nothing to write -- and writing
		// it would actively hurt: a host that quantises hands back a ROUNDED
		// copy of our own value, params[] would take the rounding, and the
		// "did a covered parameter move?" test above works to a tighter
		// tolerance than this one and would read that rounding as an edit.
		return true;
	}

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log
	// nobody reads.
	return true;
}

void FiveWire::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float FiveWire::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

FFResult FiveWire::SetTextParameter( unsigned int index, const char* /*value*/ )
{
	// Display only -- there is nothing to store. It has to return FF_SUCCESS
	// all the same: see the declaration in FiveWire.h. Anything else here and
	// no real host can instantiate the plugin at all.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return FF_FAIL;
}

char* FiveWire::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

void FiveWire::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
