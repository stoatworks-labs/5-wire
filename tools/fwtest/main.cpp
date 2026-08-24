/**
    fwtest -- drive 5-wire offline, and check that the cable is a cable.

    Driving Resolume from an agent session is not reliable, and "it compiled" is
    no evidence at all that a filter does what its comments claim. So this
    builds a headless GL 4.1 core context, drives the real FiveWire class
    through the real FFGL entry sequence, and reads the frame back as floats.

    The checks are the part worth having:

      --kernel   the cable's response against the law it claims to obey. Unit
                 sum, strictly causal, and a frequency response that really is
                 exp(-alpha*sqrt(pi*f)) -- measured by transforming the taps
                 the plugin would upload, not by asserting the formula twice.
      --eq       the equaliser really is the inverse of that response: h * e
                 back to an impulse, to a stated residual, with unit DC gain.
      --impulse  END TO END. One bright column through the whole chain, read
                 back a row, and compare it with Cable.cpp tap by tap. This is
                 the check that covers the upload: a kernel the GPU never
                 received renders a perfectly plausible picture.
      --ghost    the repeats land at twice the transit time and at the product
                 of the two reflection coefficients. Measured off the frame.
      --presets  three host behaviours, no GL. Resolume does not consume value
                 events, and a copy-based preset loses to its own echo.

    And the renders:

        fwtest --list
        fwtest --report
        fwtest --out /tmp/frame.png --width 1280 --height 720
        fwtest --set "Length=0.8" --set "Cable Type=3" --frames 30
        ffmpeg ... -f rawvideo -pix_fmt rgba - | fwtest --pipe --width W --height H | ffmpeg ...
*/

#include "FiveWire.h"

#include "Cable.h"
#include "Controls.h"
#include "Presets.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace fivewire;

namespace
{
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, std::uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< std::uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< std::uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< std::uint32_t >( width ) );
	putU32( ihdr, static_cast< std::uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test picture.
//
// Chosen so a wrong answer is visible rather than so it looks nice. Each band
// answers a different question about a long cable:
//
//   single columns   the impulse response itself: how far it smears, which
//                    way, and where the repeats land
//   colour bars      skew puts the three conductors' edges in different
//                    places, and crosstalk outlines them
//   bright block     what follows it is streaking, which is the response's
//                    tail and nothing else
//   1px stripes      the bandwidth, where a soft cable simply gives up
//   flat grey        hum, ingress and noise, with nothing in the way
//---------------------------------------------------------------------------
void setPixel( std::vector< unsigned char >& image, int width, int height, int x, int y, float r, float g, float b )
{
	//y from the top, which is how the pattern below is described and how the
	//PNG will be read. GL puts row zero at the bottom, so the flip lives here.
	const size_t i   = ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4;
	image[ i + 0 ]   = static_cast< unsigned char >( std::lround( std::clamp( r, 0.0f, 1.0f ) * 255.0f ) );
	image[ i + 1 ]   = static_cast< unsigned char >( std::lround( std::clamp( g, 0.0f, 1.0f ) * 255.0f ) );
	image[ i + 2 ]   = static_cast< unsigned char >( std::lround( std::clamp( b, 0.0f, 1.0f ) * 255.0f ) );
	image[ i + 3 ]   = 255;
}

std::vector< unsigned char > buildTestPicture( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			setPixel( image, width, height, x, y, 0.0f, 0.0f, 0.0f );

	const int band = std::max( 1, height / 5 );

	for( int y = 0; y < height; ++y )
	{
		const int which = std::min( 4, y / band );
		for( int x = 0; x < width; ++x )
		{
			const float u = static_cast< float >( x ) / static_cast< float >( width );

			if( which == 0 )
			{
				//Three isolated columns, well apart, so a repeat off one does
				//not land on another.
				const bool on = ( x == width / 8 ) || ( x == width / 2 ) || ( x == ( 3 * width ) / 4 );
				if( on )
					setPixel( image, width, height, x, y, 1.0f, 1.0f, 1.0f );
			}
			else if( which == 1 )
			{
				const int bar          = std::min( 7, static_cast< int >( u * 8.0f ) );
				const float bars[ 8 ][ 3 ] = {
					{ 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 },
					{ 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0 }
				};
				setPixel( image, width, height, x, y, bars[ bar ][ 0 ], bars[ bar ][ 1 ], bars[ bar ][ 2 ] );
			}
			else if( which == 2 )
			{
				//A bright block with a long black run after it. Everything
				//visible in that run is the tail of the cable's response.
				const float v = ( u > 0.10f && u < 0.30f ) ? 1.0f : 0.0f;
				setPixel( image, width, height, x, y, v, v, v );
			}
			else if( which == 3 )
			{
				const float v = ( x % 2 == 0 ) ? 1.0f : 0.0f;
				setPixel( image, width, height, x, y, v, v, v );
			}
			else
			{
				setPixel( image, width, height, x, y, 0.45f, 0.45f, 0.45f );
			}
		}
	}

	return image;
}

/// Black with single white columns, for the measurements.
std::vector< unsigned char > buildImpulsePicture( int width, int height, int column )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			setPixel( image, width, height, x, y, 0.0f, 0.0f, 0.0f );

	for( int y = 0; y < height; ++y )
		setPixel( image, width, height, column, y, 1.0f, 1.0f, 1.0f );

	return image;
}

//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

//---------------------------------------------------------------------------
// A rig: one plugin, one input texture, one float output.
//---------------------------------------------------------------------------
class Rig
{
public:
	bool start( int w, int h, const std::vector< unsigned char >& picture )
	{
		width  = w;
		height = h;

		glGenTextures( 1, &inputTexture );
		glBindTexture( GL_TEXTURE_2D, inputTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, picture.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );

		//Float, not 8-bit. Half the numbers this harness measures -- the tail
		//of the response, a fourth-bounce ghost -- are a few thousandths, and
		//a byte target quantises exactly the region worth measuring.
		glGenTextures( 1, &outputTexture );
		glBindTexture( GL_TEXTURE_2D, outputTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			std::fprintf( stderr, "fwtest: output framebuffer is incomplete\n" );
			return false;
		}

		FFGLViewportStruct viewport = { 0, 0, static_cast< FFUInt32 >( width ), static_cast< FFUInt32 >( height ) };
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "fwtest: InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = inputTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;

		return true;
	}

	void replacePicture( const std::vector< unsigned char >& picture )
	{
		glBindTexture( GL_TEXTURE_2D, inputTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool render( int frames, double fps )
	{
		for( int frame = 0; frame < frames; ++frame )
		{
			const double seconds = static_cast< double >( frame ) / fps;
			plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
			plugin.SetTime( seconds );

			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );

			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "fwtest: ProcessOpenGL failed on frame %d\n", frame );
				return false;
			}
		}
		return true;
	}

	std::vector< float > readFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// One row, in picture order, red channel only.
	std::vector< float > readRow( int yFromTop )
	{
		const std::vector< float > all = readFloat();
		const int glRow                = height - 1 - yFromTop;
		std::vector< float > row( width );
		for( int x = 0; x < width; ++x )
			row[ x ] = all[ ( static_cast< size_t >( glRow ) * width + x ) * 4 ];
		return row;
	}

	int indexOfParameter( const std::string& name )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
				return static_cast< int >( i );
		}
		return -1;
	}

	bool set( const std::string& name, float value )
	{
		const int index = indexOfParameter( name );
		if( index < 0 )
		{
			std::fprintf( stderr, "fwtest: no parameter named '%s' (try --list)\n", name.c_str() );
			return false;
		}
		plugin.SetFloatParameter( static_cast< unsigned int >( index ), value );
		return true;
	}

	FiveWire plugin;
	int width  = 0;
	int height = 0;

private:
	GLuint inputTexture  = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};
};

/// Everything off except the cable, so a measurement measures one thing.
bool quietenEverything( Rig& rig )
{
	const std::pair< const char*, float > quiet[] = {
		{ "Ghosting", 0.0f }, { "Skew", 0.0f }, { "Crosstalk", 0.0f },
		{ "Noise", 0.0f }, { "Hum", 0.0f }, { "Ingress", 0.0f },
		{ "Jitter", 0.0f }, { "Sync Level", 1.0f }, { "Sync On Green", 0.0f },
		{ "Gain", 0.5f }, { "Red", 0.5f }, { "Green", 0.5f }, { "Blue", 0.5f },
		{ "Pre-Emphasis", 0.0f }, { "Cable EQ", 0.0f },
		{ "Output Gain", 0.5f }, { "Black Level", 0.5f }, { "DC Restore", 1.0f },
		{ "Sample Phase", 0.0f },
	};

	for( const auto& entry : quiet )
		if( !rig.set( entry.first, entry.second ) )
			return false;

	return true;
}

//===========================================================================
// --kernel: the cable's response against the law it claims to obey.
//
// What is asserted here and what is only reported is a real distinction, not a
// choice of tolerances.
//
// EXACT, and asserted as such: no cable is no filter; the response sums to one
// because a passive cable does not dim a picture; every tap is positive and to
// the right of the origin, because nothing arrives before it was sent and a
// passive cable cannot make a dark pixel darker; and the magnitude response
// never rises with frequency and never exceeds one.
//
// APPROXIMATE, and reported with a wide bound: the -3 dB bandwidth. The kernel
// is the continuous response integrated over pixel bins and truncated at 640
// pixels, and both of those bite -- the bins carry the pixel's own aperture,
// and the truncated tail is given back by renormalising, which lifts the low
// end. Together they drift the measured bandwidth from about 0.85 of the
// analytic figure at short lengths to about 1.2 at long ones. That drift is
// the discretisation and is stated here rather than hidden in a tolerance.
// The bound is still tight enough for the thing it exists to catch: a loss
// that went as f rather than sqrt(f) would put the ratio out by more than
// three to one across this sweep.
//===========================================================================
int checkKernel()
{
	int failures = 0;

	std::printf( "the cable's own response\n\n" );

	//A pure identity first: no cable at all must be exactly no filter.
	{
		const Kernel k = lossKernel( 0.0f );
		if( k.tap[ 0 ] != 1.0f )
		{
			std::printf( "  FAIL: zero length is not the identity (tap0 = %.9f)\n", k.tap[ 0 ] );
			++failures;
		}
		for( int n = 1; n < kTaps; ++n )
			if( k.tap[ n ] != 0.0f )
			{
				std::printf( "  FAIL: zero length has a tap at %d (%.9f)\n", n, k.tap[ n ] );
				++failures;
				break;
			}
	}

	//The closed form in bandwidthCyclesPerPixel, checked against the law it
	//was solved out of. This one IS exact -- it is arithmetic, not a
	//discretisation.
	for( float alpha : { 0.3f, 1.0f, 2.5f } )
	{
		const double f    = bandwidthCyclesPerPixel( alpha );
		const double gain = std::exp( -static_cast< double >( alpha ) * std::sqrt( kPi * f ) );
		if( std::fabs( gain - 1.0 / std::sqrt( 2.0 ) ) > 1.0e-6 )
		{
			std::printf( "  FAIL: bandwidthCyclesPerPixel(%.1f) gives %.6f of the signal, not 0.707107\n",
			             alpha, gain );
			++failures;
		}
	}

	std::printf( "  alpha    tap0     sum        head     f(-3dB)   analytic   ratio\n" );

	for( float alpha : { 0.05f, 0.15f, 0.3f, 0.4f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f } )
	{
		const Kernel k = lossKernel( alpha );

		double sum = 0.0;
		for( float t : k.tap )
			sum += t;
		const double head = sum;
		for( int level = 0; level < kWideLevels; ++level )
			for( int j = 0; j < kWideTaps; ++j )
				sum += k.wide[ level ][ j ];

		if( std::fabs( sum - 1.0 ) > 1.0e-5 )
		{
			std::printf( "  FAIL: alpha %.2f sums to %.9f\n", alpha, sum );
			++failures;
		}

		for( int n = 0; n < kTaps; ++n )
			if( k.tap[ n ] < 0.0f )
			{
				std::printf( "  FAIL: alpha %.2f has a negative tap at %d\n", alpha, n );
				++failures;
				break;
			}

		//The magnitude response of exactly what the plugin uploads: 64 taps
		//and sixteen box weights, each box carrying its own sinc because the
		//reduction really does average over that many pixels.
		auto magnitude = [ & ]( double f ) {
			std::complex< double > H( 0.0, 0.0 );
			for( int n = 0; n < kTaps; ++n )
				H += std::complex< double >( k.tap[ n ] )
				     * std::exp( std::complex< double >( 0.0, -2.0 * kPi * f * n ) );

			for( int level = 0; level < kWideLevels; ++level )
			{
				const double stride = kWideStride[ level ];
				const double start  = kWideStart[ level ];
				for( int j = 0; j < kWideTaps; ++j )
				{
					const double centre = start + stride * j + 0.5 * stride;
					const double boxArg = kPi * f * stride;
					const double box    = ( std::fabs( boxArg ) < 1.0e-9 ) ? 1.0 : std::sin( boxArg ) / boxArg;
					H += std::complex< double >( k.wide[ level ][ j ] * box )
					     * std::exp( std::complex< double >( 0.0, -2.0 * kPi * f * centre ) );
				}
			}
			return std::abs( H );
		};

		//Never a gain, anywhere. A kernel that rises with frequency is not a
		//lossy cable however well it fits a curve.
		double previous = 2.0;
		for( double f = 0.0; f <= 0.5; f += 0.002 )
		{
			const double m = magnitude( f );
			if( m > previous + 2.0e-3 || m > 1.0 + 1.0e-6 )
			{
				std::printf( "  FAIL: alpha %.2f gains with frequency at f = %.3f\n", alpha, f );
				++failures;
				break;
			}
			previous = m;
		}

		//Where it is 3 dB down, by bisection over the same response.
		double lo = 1.0e-4;
		double hi = 0.49;
		for( int i = 0; i < 50; ++i )
		{
			const double mid = 0.5 * ( lo + hi );
			if( magnitude( mid ) > 0.70710678 )
				lo = mid;
			else
				hi = mid;
		}
		const double measured = 0.5 * ( lo + hi );
		const double analytic = bandwidthCyclesPerPixel( alpha );
		const bool inBand     = measured < 0.45 && analytic < 0.45;
		const double ratio    = analytic > 0.0 ? measured / analytic : 0.0;

		std::printf( "  %5.2f   %.4f   %.6f   %.4f   %.5f   %.5f    %s%.3f\n",
		             alpha, k.tap[ 0 ], sum, head, measured, analytic,
		             inBand ? "" : "off-band ", ratio );

		//Only where the pixel grid can express the answer at all. Below about
		//alpha 0.35 the cable outruns Nyquist -- which is the whole point of
		//a short run -- and a bandwidth measured off a grid that cannot reach
		//it is a fact about the grid.
		if( inBand && ( ratio < 0.7 || ratio > 1.4 ) )
		{
			std::printf( "  FAIL: alpha %.2f is %.2f times its analytic bandwidth\n", alpha, ratio );
			++failures;
		}
	}

	std::printf( failures == 0 ? "\nkernel: PASS\n" : "\nkernel: FAIL\n" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --eq: the equaliser really is the inverse of the cable.
//
// The assertion that matters most here is the one that looks least like a
// specification: the cascade must never be SOFTER than the cable on its own.
// The first version of this equaliser was a frequency-domain Wiener inverse,
// which is the mathematically optimal filter and which degenerates into a
// matched filter where the cable has thrown the signal away -- so at a hundred
// metres, Cable EQ at maximum made the picture softer than leaving it alone.
// Every other number below was healthy while that was true.
//===========================================================================
int checkEqualiser()
{
	std::printf( "the equaliser against the cable it inverts\n\n" );
	std::printf( "  alpha   DC gain    peak tap   residual   sharpened by\n" );

	int failures         = 0;
	double lastResidual  = 0.0;

	for( float alpha : { 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f } )
	{
		const Kernel k = lossKernel( alpha );

		float e[ kTaps ] = {};
		equaliserKernel( alpha, 1.0f, e );

		float combined[ kTaps ] = {};
		convolve( k.tap, e, combined );

		double dc = 0.0;
		for( float t : e )
			dc += t;

		//How far the cascade is from an impulse. Never zero: the equaliser is
		//64 taps and the cable's tail is longer than that, which is the exact
		//sense in which a real equaliser cannot fully undo a long run either.
		double residual = std::fabs( combined[ 0 ] - 1.0f );
		for( int n = 1; n < kTaps; ++n )
			residual = std::max( residual, static_cast< double >( std::fabs( combined[ n ] ) ) );

		double peak = 0.0;
		for( float t : e )
			peak = std::max( peak, static_cast< double >( std::fabs( t ) ) );

		const double sharpening = combined[ 0 ] / k.tap[ 0 ];

		std::printf( "  %5.2f   %.6f   %7.3f    %.5f    %.2fx\n",
		             alpha, dc, peak, residual, sharpening );

		//Unit DC gain is not negotiable: an equaliser changes the balance of a
		//picture, never its brightness.
		if( std::fabs( dc - 1.0 ) > 1.0e-4 )
		{
			std::printf( "  FAIL: alpha %.2f equaliser has DC gain %.6f\n", alpha, dc );
			++failures;
		}

		//The one that catches the Wiener version. An equaliser may run out of
		//range; it may not go the other way.
		if( sharpening < 1.0 )
		{
			std::printf( "  FAIL: alpha %.2f -- the equaliser made the picture SOFTER (%.3f of the cable's own peak)\n",
			             alpha, sharpening );
			++failures;
		}

		//Over the range where a 64-tap filter can genuinely answer the cable.
		//Past alpha 1.2 it cannot, and the honest thing is the number in the
		//table rather than a tolerance wide enough to cover it.
		if( alpha <= 1.0f && residual > 0.10 )
		{
			std::printf( "  FAIL: alpha %.2f leaves %.4f of a recoverable response uncorrected\n", alpha, residual );
			++failures;
		}

		//And it must get worse with length, monotonically. A design that got
		//BETTER at a longer cable would mean the length is not reaching it.
		if( residual < lastResidual - 1.0e-6 )
		{
			std::printf( "  FAIL: alpha %.2f corrects better than the shorter cable before it\n", alpha );
			++failures;
		}
		lastResidual = residual;
	}

	//Amount zero must be exactly flat, not nearly flat: it is the setting the
	//plugin sits at with the equaliser switched off, and a filter that is
	//nearly an impulse is a filter.
	{
		float e[ kTaps ] = {};
		equaliserKernel( 1.0f, 0.0f, e );
		if( e[ 0 ] != 1.0f )
		{
			std::printf( "  FAIL: amount 0 is not flat (tap0 = %.9f)\n", e[ 0 ] );
			++failures;
		}
		for( int n = 1; n < kTaps; ++n )
			if( e[ n ] != 0.0f )
			{
				std::printf( "  FAIL: amount 0 has a tap at %d\n", n );
				++failures;
				break;
			}
	}

	std::printf( failures == 0 ? "\nequaliser: PASS\n" : "\nequaliser: FAIL\n" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --impulse: end to end. One column in, the kernel out.
//===========================================================================
int checkImpulse()
{
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "fwtest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	const int W = 1024;
	const int H = 64;
	const int column = 64;

	std::printf( "one bright column through the whole chain, against Cable.cpp\n\n" );
	std::printf( "  type  length   alpha   worst tap error   energy\n" );

	int failures = 0;

	//Two short runs, where the head holds the response and the comparison is
	//tap by tap, and two long ones, where it does not and the thing worth
	//checking is that the tail did not go missing.
	const struct
	{
		float type;
		float length;
	} cases[] = { { 0.0f, 0.35f }, { 1.0f, 0.25f }, { 2.0f, 0.60f }, { 1.0f, 0.75f } };

	for( const auto& c : cases )
	{
		Rig rig;
		if( !rig.start( W, H, buildImpulsePicture( W, H, column ) ) )
			return 1;
		if( !quietenEverything( rig ) )
			return 1;
		rig.set( "Cable Type", c.type );
		rig.set( "Length", c.length );
		rig.set( "EQ Length", c.length );

		if( !rig.render( 1, 60.0 ) )
			return 1;

		const std::vector< float > row = rig.readRow( H / 2 );

		const controls::Drive drive = rig.plugin.DriveForTest( 0.0f, 1.0f / 60.0f, static_cast< float >( W ) );

		double worst = 0.0;
		int worstTap = 0;
		for( int n = 0; n < kTaps; ++n )
		{
			const double error = std::fabs( static_cast< double >( row[ column + n ] ) - drive.cableKernel[ n ] );
			if( error > worst )
			{
				worst    = error;
				worstTap = n;
			}
		}

		//Everything to the right of the column, which is the whole response
		//including the part read out of the reduced buffers.
		double energy = 0.0;
		for( int x = column; x < W; ++x )
			energy += row[ x ];

		std::printf( "  %4.0f  %.3f   %.4f   %.6f at %-3d    %.4f\n",
		             c.type, c.length, drive.alpha, worst, worstTap, energy );

		//A thousandth of a level. The taps are uploaded as floats and applied
		//to an 8-bit input in a 16-bit float buffer, so this is nearly exact
		//and anything looser would let a wrong kernel through.
		if( worst > 1.0e-3 )
		{
			std::printf( "  FAIL: the GPU is not convolving with the kernel Cable.cpp computed\n" );
			++failures;
		}

		//And the picture did not dim. A response that sums to one has to
		//deliver one, and if the tail were dropped rather than read this is
		//where it would show.
		if( std::fabs( energy - 1.0 ) > 0.02 )
		{
			std::printf( "  FAIL: the response delivers %.4f of what went in\n", energy );
			++failures;
		}
	}

	std::printf( failures == 0 ? "\nimpulse: PASS\n" : "\nimpulse: FAIL\n" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --ghost: the repeats land where the transit time says they do.
//===========================================================================
int checkGhost()
{
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "fwtest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	const int W      = 1600;
	const int H      = 32;
	const int column = 40;

	std::printf( "where the repeats land, against twice the transit time\n\n" );
	std::printf( "  length   predicted   measured   error    amplitude  predicted\n" );

	int failures = 0;

	for( float length : { 0.35f, 0.5f, 0.65f } )
	{
		Rig rig;
		if( !rig.start( W, H, buildImpulsePicture( W, H, column ) ) )
			return 1;
		if( !quietenEverything( rig ) )
			return 1;

		//A properly coax run so the repeat is sharp enough to find, an open
		//far end and an amplifier with no back-match, which is the ghost at
		//its plainest.
		rig.set( "Cable Type", 0.0f );
		rig.set( "Length", length );
		rig.set( "EQ Length", length );
		rig.set( "Termination", 1.0f );
		rig.set( "Ghosting", 1.0f );
		rig.set( "Bounces", 0.0f );

		if( !rig.render( 1, 60.0 ) )
			return 1;

		const std::vector< float > row = rig.readRow( H / 2 );
		const controls::Drive drive    = rig.plugin.DriveForTest( 0.0f, 1.0f / 60.0f, static_cast< float >( W ) );

		//Find the brightest thing at least ten pixels clear of the picture
		//itself. Ten because the picture's own smear is a few pixels wide and
		//a peak inside it is not a repeat.
		int peak       = -1;
		float peakValue = 0.0f;
		for( int x = column + 10; x < W; ++x )
		{
			if( row[ x ] > peakValue )
			{
				peakValue = row[ x ];
				peak      = x;
			}
		}

		const float predicted = drive.ghostOffsetPx[ 0 ];
		const float measured  = static_cast< float >( peak - column );
		const float error     = std::fabs( measured - predicted );

		std::printf( "  %.3f    %7.2f     %7.2f    %5.2f    %.4f     %.4f\n",
		             length, predicted, measured, error, peakValue, drive.ghostAmp[ 0 ] );

		//Two pixels. The repeat has been through the cable twice more than the
		//picture, so its peak sits slightly late of the arithmetic -- which is
		//a real property of the thing rather than an error in it.
		if( error > 2.0f )
		{
			std::printf( "  FAIL: the repeat is not where the transit time puts it\n" );
			++failures;
		}

		if( peakValue < drive.ghostAmp[ 0 ] * 0.15f )
		{
			std::printf( "  FAIL: the repeat is far too faint to be the reflection\n" );
			++failures;
		}
	}

	//And with the amplifier properly back-matched there must be no repeat at
	//all, whatever the far end is doing. One mismatch is not a ghost.
	{
		Rig rig;
		if( !rig.start( W, H, buildImpulsePicture( W, H, column ) ) )
			return 1;
		if( !quietenEverything( rig ) )
			return 1;
		rig.set( "Cable Type", 0.0f );
		rig.set( "Length", 0.5f );
		rig.set( "EQ Length", 0.5f );
		rig.set( "Termination", 1.0f );
		rig.set( "Ghosting", 0.0f );
		if( !rig.render( 1, 60.0 ) )
			return 1;

		const std::vector< float > row = rig.readRow( H / 2 );
		float worst                    = 0.0f;
		for( int x = column + 10; x < W; ++x )
			worst = std::max( worst, row[ x ] );

		std::printf( "\n  back-matched amplifier, open far end: brightest repeat %.6f\n", worst );
		if( worst > 0.02f )
		{
			std::printf( "  FAIL: a ghost needs two mismatches and this one only has one\n" );
			++failures;
		}
	}

	std::printf( failures == 0 ? "\nghost: PASS\n" : "\nghost: FAIL\n" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --presets: three host behaviours, no GL.
//
// Resolume does NOT consume FF_EVENT_FLAG_VALUE. It keeps pushing the values
// it still believes in, and a copy-based preset applier reads its own host's
// echo as an operator edit and drops back to Custom before a single frame has
// been rendered. This is the test that fails on that code, in exactly the
// "ignores" column.
//===========================================================================
int checkPresets()
{
	enum Host
	{
		Honours,
		Ignores,
		Quantises
	};
	const char* const hostNames[] = { "honours events", "ignores events", "quantises" };

	int count                     = 0;
	const unsigned int* coveredIds = FiveWire::PresetParamIDsForTest( count );

	int failures = 0;

	std::printf( "factory presets against three host behaviours\n\n" );

	for( int host = Honours; host <= Quantises; ++host )
	{
		int bad = 0;

		for( int p = 1; p <= presets::kCount; ++p )
		{
			FiveWire plugin;

			//Find the Preset parameter the way a host would.
			int presetId = -1;
			for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			{
				const char* name = plugin.GetParamName( i );
				if( name != nullptr && std::string( name ) == "Preset" )
					presetId = static_cast< int >( i );
			}
			if( presetId < 0 )
			{
				std::printf( "  FAIL: there is no Preset parameter\n" );
				return 1;
			}

			//The host's opening position: it pushes every default once.
			std::vector< float > believed( plugin.GetNumParams(), 0.0f );
			for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
				believed[ i ] = plugin.GetFloatParameter( i );
			for( int j = 0; j < count; ++j )
				plugin.SetFloatParameter( coveredIds[ j ], believed[ coveredIds[ j ] ] );

			//The operator picks a preset.
			plugin.SetFloatParameter( static_cast< unsigned int >( presetId ), static_cast< float >( p ) );

			//And the host answers, in its own way.
			for( int j = 0; j < count; ++j )
			{
				const unsigned int id = coveredIds[ j ];
				float pushed          = believed[ id ];

				if( host == Honours )
					pushed = plugin.GetFloatParameter( id );
				else if( host == Quantises )
					pushed = std::round( plugin.GetFloatParameter( id ) * 1000.0f ) / 1000.0f;

				plugin.SetFloatParameter( id, pushed );
			}

			//The dropdown must still say what the operator chose.
			const int stillSelected = static_cast< int >( std::lround( plugin.GetFloatParameter( presetId ) ) );
			if( stillSelected != p )
			{
				std::printf( "  FAIL [%s] preset %d (%s) fell back to %d\n",
				             hostNames[ host ], p, presets::kPresets[ p - 1 ].name, stillSelected );
				++bad;
				continue;
			}

			//And the values must be the preset's.
			for( int j = 0; j < count; ++j )
			{
				const float wanted = presets::kPresets[ p - 1 ].v[ j ];
				const float got    = plugin.GetFloatParameter( coveredIds[ j ] );
				if( std::fabs( wanted - got ) > 2.0e-3f )
				{
					std::printf( "  FAIL [%s] preset %d (%s): parameter %u is %.4f, wanted %.4f\n",
					             hostNames[ host ], p, presets::kPresets[ p - 1 ].name,
					             coveredIds[ j ], got, wanted );
					++bad;
					break;
				}
			}
		}

		std::printf( "  %-16s %d of %d presets survive\n", hostNames[ host ], presets::kCount - bad, presets::kCount );
		failures += bad;
	}

	std::printf( failures == 0 ? "\npresets: PASS\n" : "\npresets: FAIL\n" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Parameter automation for --pipe.
//
// A plain text file of `frame  Parameter Name  value` lines. Values are held
// before the first key and after the last, and linearly interpolated between.
// This is what the video pipeline drives the plugin with: the cue sheet IS the
// edit, so a beat is moved by moving a number rather than by re-rendering to a
// different plan.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a   = track[ i - 1 ];
			const auto& b   = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
	    "fwtest -- drive 5-wire offline\n"
	    "\n"
	    "  --list                 every parameter and its current value\n"
	    "  --report               what the current settings mean in metres and megahertz\n"
	    "  --set \"Name=Value\"     set a control by its display name (repeatable)\n"
	    "  --out PATH             write a PNG (default /tmp/5-wire.png)\n"
	    "  --width N --height N   frame size (default 1280x720)\n"
	    "  --frames N             render N frames before reading back (default 8)\n"
	    "  --fps N                what to tell the plugin the frame rate is\n"
	    "  --impulse-picture      render a single column instead of the test card\n"
	    "  --pipe                 rawvideo rgba on stdin, rawvideo rgba on stdout\n"
	    "  --script FILE          animate controls under --pipe: `frame Name value` lines\n"
	    "\n"
	    "  --kernel               the cable's response against exp(-alpha*sqrt(pi*f))\n"
	    "  --eq                   the equaliser against the cable it inverts\n"
	    "  --impulse              end to end: the GPU's convolution against Cable.cpp\n"
	    "  --ghost                where the repeats land, measured off the frame\n"
	    "  --presets              factory presets against three host behaviours\n" );
}

bool readExactly( void* buffer, size_t bytes )
{
	size_t done = 0;
	auto* out   = static_cast< unsigned char* >( buffer );
	while( done < bytes )
	{
		const size_t got = fread( out + done, 1, bytes - done, stdin );
		if( got == 0 )
			return false;
		done += got;
	}
	return true;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/5-wire.png";
	int width              = 1280;
	int height             = 720;
	int frames             = 8;
	bool listOnly          = false;
	bool report            = false;
	bool pipeMode          = false;
	bool impulsePicture    = false;
	double fps             = 60.0;
	std::string scriptPath;
	std::vector< std::pair< std::string, float > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--kernel" )
			return checkKernel();
		else if( arg == "--eq" )
			return checkEqualiser();
		else if( arg == "--impulse" )
			return checkImpulse();
		else if( arg == "--ghost" )
			return checkGhost();
		else if( arg == "--presets" )
			return checkPresets();
		else if( arg == "--out" )
			outputPath = next();
		else if( arg == "--width" )
			width = std::atoi( next().c_str() );
		else if( arg == "--height" )
			height = std::atoi( next().c_str() );
		else if( arg == "--frames" )
			frames = std::atoi( next().c_str() );
		else if( arg == "--fps" )
			fps = std::strtod( next().c_str(), nullptr );
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--report" )
			report = true;
		else if( arg == "--pipe" )
			pipeMode = true;
		else if( arg == "--impulse-picture" )
			impulsePicture = true;
		else if( arg == "--script" )
			scriptPath = next();
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals          = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "fwtest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "fwtest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "fwtest: width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( listOnly || report )
	{
		FiveWire plugin;

		for( const auto& override : overrides )
		{
			int index = -1;
			for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			{
				const char* name = plugin.GetParamName( i );
				if( name != nullptr && override.first == name )
					index = static_cast< int >( i );
			}
			if( index < 0 )
			{
				std::fprintf( stderr, "fwtest: no parameter named '%s'\n", override.first.c_str() );
				return 2;
			}
			plugin.SetFloatParameter( static_cast< unsigned int >( index ), override.second );
		}

		if( listOnly )
		{
			for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
				std::printf( "%2u  %-20s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
			return 0;
		}

		const controls::Drive d = plugin.DriveForTest( 0.0f, 1.0f / 60.0f, static_cast< float >( width ) );

		std::printf( "cable        %.1f m\n", d.metres );
		std::printf( "loss alpha   %.4f sqrt(pixels)\n", d.alpha );
		std::printf( "bandwidth    %.4f cycles/pixel  (-3 dB)\n", d.bandwidth );
		std::printf( "transit      %.2f pixels one way\n", d.transitPx );
		std::printf( "taps used    %d cable, %d amplifier, %d equaliser\n", d.cableTaps, d.headTaps, d.eqTaps );
		std::printf( "tail buffers %s\n", d.useWide ? "yes" : "no" );
		std::printf( "skew         %+.2f %+.2f %+.2f pixels\n", d.skewPx[ 0 ], d.skewPx[ 1 ], d.skewPx[ 2 ] );
		for( int i = 0; i < d.ghostCount; ++i )
			std::printf( "repeat %d     %+.4f at %.1f px, softened by %.1f px\n",
			             i + 1, d.ghostAmp[ i ], d.ghostOffsetPx[ i ], d.ghostBlurPx[ i ] );
		if( d.ghostCount == 0 )
			std::printf( "repeats      none\n" );
		std::printf( "sync margin  %.3f drive - %.3f cable loss\n", d.syncDrive, d.syncLoss );
		if( d.rollOffset > 0.0f )
			std::printf( "             the receiver has lost vertical lock\n" );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "fwtest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	//In pipe mode stdout carries the video, so everything conversational has to
	//go to stderr or it ends up inside a frame.
	std::fprintf( pipeMode ? stderr : stdout, "GL %s / %s\n",
	              glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	const std::vector< unsigned char > picture =
	    impulsePicture ? buildImpulsePicture( width, height, width / 8 ) : buildTestPicture( width, height );

	Rig rig;
	if( !rig.start( width, height, picture ) )
		return 1;

	for( const auto& override : overrides )
		if( !rig.set( override.first, override.second ) )
			return 2;

	if( pipeMode )
	{
		std::map< std::string, Track > tracks;
		if( !scriptPath.empty() )
		{
			std::string error;
			tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "fwtest: %s\n", error.c_str() );
				return 2;
			}
			//Fail on a name that does not exist rather than silently animating
			//nothing for a minute of footage.
			for( const auto& entry : tracks )
			{
				if( rig.indexOfParameter( entry.first ) < 0 )
				{
					std::fprintf( stderr, "fwtest: script names '%s', which is not a parameter (try --list)\n",
					              entry.first.c_str() );
					return 2;
				}
			}
		}

		const size_t frameBytes = static_cast< size_t >( width ) * height * 4;
		const size_t rowBytes   = static_cast< size_t >( width ) * 4;
		std::vector< unsigned char > in( frameBytes );
		std::vector< unsigned char > flip( frameBytes );
		std::vector< unsigned char > out( frameBytes );

		long frame = 0;
		while( readExactly( in.data(), frameBytes ) )
		{
			//ffmpeg hands over top-down rows; GL wants the bottom row first.
			for( int y = 0; y < height; ++y )
				std::memcpy( flip.data() + static_cast< size_t >( y ) * rowBytes,
				             in.data() + static_cast< size_t >( height - 1 - y ) * rowBytes, rowBytes );
			rig.replacePicture( flip );

			for( const auto& entry : tracks )
				rig.plugin.SetFloatParameter(
				    static_cast< unsigned int >( rig.indexOfParameter( entry.first ) ),
				    valueAt( entry.second, static_cast< int >( frame ) ) );

			const double seconds = static_cast< double >( frame ) / fps;
			rig.plugin.SetClockScaleForTest( 1.0 );
			rig.plugin.SetTime( seconds );
			if( !rig.render( 1, fps ) )
				return 1;

			const std::vector< float > pixels = rig.readFloat();
			for( int y = 0; y < height; ++y )
			{
				const int glRow = height - 1 - y;
				for( int x = 0; x < width; ++x )
					for( int c = 0; c < 4; ++c )
					{
						const float v = pixels[ ( static_cast< size_t >( glRow ) * width + x ) * 4 + c ];
						out[ static_cast< size_t >( y ) * rowBytes + x * 4 + c ] =
						    static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
					}
			}

			fwrite( out.data(), 1, frameBytes, stdout );
			++frame;
		}

		return 0;
	}

	if( !rig.render( frames, fps ) )
		return 1;

	const std::vector< float > pixels = rig.readFloat();
	std::vector< unsigned char > rgba( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < rgba.size(); ++i )
		rgba[ i ] = static_cast< unsigned char >( std::lround( std::clamp( pixels[ i ], 0.0f, 1.0f ) * 255.0f ) );

	//Flip into picture order for the PNG.
	std::vector< unsigned char > flipped( rgba.size() );
	const size_t rowBytes = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * rowBytes,
		             rgba.data() + static_cast< size_t >( height - 1 - y ) * rowBytes, rowBytes );

	if( !writePng( outputPath, width, height, flipped ) )
	{
		std::fprintf( stderr, "fwtest: could not write %s\n", outputPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s\n", outputPath.c_str() );
	return 0;
}
