#include "Cable.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fivewire
{
namespace
{
//---------------------------------------------------------------------------
// The five kinds of run this plugin knows about.
//
// Ordered by how much they hurt, because that is how the dropdown will be
// read. The loss figures are the published ones for the cable each entry
// stands for; the skew figures are the ones that separate the families.
//---------------------------------------------------------------------------
constexpr CableSpec kCables[] = {
	// Five separate 75 ohm coaxes on BNC. What a fixed installation is made
	// of, and near enough transparent at any length a room has.
	{ "RGBHV Coax", 1.10f, 0.66f, 1.0f, { 0.6f, 0.0f, -0.6f }, 0.120f, 1.00f },

	// The moulded lead in the flight case. Thin conductors, a foil wrap and a
	// hood full of unshielded pigtails: three times the loss of proper coax
	// and far more of the room gets in.
	{ "VGA Lead", 3.30f, 0.68f, 3.0f, { 0.5f, 0.0f, -0.5f }, 0.660f, 0.45f },

	// Mini-coax breakout, the compromise: real coax, but 28 AWG of it.
	{ "Mini-Coax", 2.20f, 0.70f, 2.0f, { 0.5f, 0.0f, -0.5f }, 0.260f, 0.75f },

	// UTP with passive baluns. The pairs are twisted at deliberately
	// different rates so they do not couple, which means they are genuinely
	// different lengths -- tens of nanoseconds of skew over a long run, and
	// the reason a CAT5 VGA extender fringes colour on vertical edges that
	// coax at the same length does not.
	{ "CAT5 Balun", 2.00f, 0.64f, 45.0f, { 1.0f, -0.2f, -0.8f }, 0.850f, 0.30f },

	// Ribbon, or a hand-made loom. No shield worth the name and the
	// conductors run parallel for their whole length, which is the definition
	// of a coupling capacitor.
	{ "Ribbon Loom", 4.50f, 0.60f, 12.0f, { 0.8f, 0.1f, -0.9f }, 1.900f, 0.08f },
};

constexpr int kCableCount = static_cast< int >( sizeof( kCables ) / sizeof( kCables[ 0 ] ) );

/// The cable equation's step response, with the t <= 0 half of it said out
/// loud. Everything in this file is a difference of two of these.
float stepAt( float alpha, float t )
{
	if( t <= 0.0f )
		return 0.0f;
	if( alpha <= 0.0f )
		return 1.0f;

	return std::erfc( alpha / ( 2.0f * std::sqrt( t ) ) );
}

/// How much of the cable's response the equaliser is designed against.
///
/// Longer than the 64 taps the equaliser itself gets, and deliberately: the
/// design has to KNOW about the tail even though it cannot fully answer it,
/// or it produces a filter that sharpens the head beautifully and leaves the
/// streak untouched.
constexpr int kDesignLength = 512;

/// The noise power the design is regularised against, relative to the signal.
///
/// This is what stops the answer being "multiply the top end by four thousand".
/// A cable that has thrown 40 dB away at the pixel rate has put that detail
/// under the receiver's own noise, and the least-squares design trades
/// sharpness against that floor exactly as a real equaliser's designer does.
/// It is why Cable EQ at maximum on a hundred-metre run gets the picture five
/// times sharper and grainy rather than twenty times sharper and clean.
constexpr double kNoiseFloor = 1.0e-4;

/// M_PI is not in the C++ standard -- MSVC hides it behind _USE_MATH_DEFINES
/// and has changed its mind about that more than once. One constant here is
/// cheaper than a portability incident in the Windows job.
constexpr double kPi = 3.14159265358979323846;
} // namespace

//---------------------------------------------------------------------------
int cableCount()
{
	return kCableCount;
}

const CableSpec& cable( int index )
{
	return kCables[ std::clamp( index, 0, kCableCount - 1 ) ];
}

//---------------------------------------------------------------------------
Kernel lossKernel( float alpha )
{
	Kernel k;

	if( alpha <= 0.0f )
	{
		k.tap[ 0 ] = 1.0f;
		k.headSum  = 1.0f;
		return k;
	}

	//The head: one pixel bin per tap, as the difference of the step response
	//across the bin. Differencing the step rather than sampling the impulse
	//is not a numerical nicety -- the impulse response is t^-3/2 and goes
	//infinite at the origin, so a point sample of it near zero is meaningless
	//while the integral over the bin is exact.
	for( int n = 0; n < kTaps; ++n )
		k.tap[ n ] = stepAt( alpha, static_cast< float >( n ) + 0.5f )
		             - stepAt( alpha, static_cast< float >( n ) - 0.5f );

	//The tail, in boxes. Same curve, coarser bins, read later from buffers
	//that have already been reduced horizontally by the same strides.
	for( int level = 0; level < kWideLevels; ++level )
	{
		const float stride = static_cast< float >( kWideStride[ level ] );
		const float start  = static_cast< float >( kWideStart[ level ] );

		for( int j = 0; j < kWideTaps; ++j )
		{
			const float a = start + stride * static_cast< float >( j );
			k.wide[ level ][ j ] = stepAt( alpha, a + stride ) - stepAt( alpha, a );
		}
	}

	//Unit DC gain, by construction rather than by hope. See the note in
	//Cable.h: a cable does not dim the picture, so whatever the truncation at
	//640 pixels threw away is given back in proportion.
	float total = 0.0f;
	for( float t : k.tap )
		total += t;
	for( int level = 0; level < kWideLevels; ++level )
		for( int j = 0; j < kWideTaps; ++j )
			total += k.wide[ level ][ j ];

	if( total > 1.0e-6f )
	{
		const float scale = 1.0f / total;
		for( float& t : k.tap )
			t *= scale;
		for( int level = 0; level < kWideLevels; ++level )
			for( int j = 0; j < kWideTaps; ++j )
				k.wide[ level ][ j ] *= scale;
	}

	k.headSum = 0.0f;
	for( float t : k.tap )
		k.headSum += t;

	return k;
}

//---------------------------------------------------------------------------
void equaliserKernel( float alpha, float amount, float out[ kTaps ] )
{
	for( int n = 0; n < kTaps; ++n )
		out[ n ] = 0.0f;
	out[ 0 ] = 1.0f;

	if( alpha <= 0.0f || amount == 0.0f )
		return;

	//The response to invert, over the whole design length rather than only the
	//part the equaliser can reach.
	std::vector< double > h( kDesignLength, 0.0 );
	for( int n = 0; n < kDesignLength; ++n )
		h[ n ] = stepAt( alpha, static_cast< float >( n ) + 0.5f )
		         - stepAt( alpha, static_cast< float >( n ) - 0.5f );

	//---------------------------------------------------------------------
	// The best kTaps-long filter that undoes it, by regularised least
	// squares: minimise |h*e - delta|^2 + lambda*|e|^2.
	//
	// Least squares and NOT an exact inverse, and this is the whole of why.
	// The exact inverse of a cable exists and can be computed by one line of
	// recursion -- and past about alpha 1.2 its coefficients grow without
	// bound, because a 64-tap filter genuinely cannot undo a response whose
	// tail is six hundred pixels long. The recursion does not fail; it
	// returns numbers of order 1e11 and the picture becomes noise. A
	// least-squares design cannot do that: it returns the best answer
	// available in 64 taps and gets gracefully worse, which is also what
	// happens to a real equaliser as the run gets longer.
	//
	// It was written twice before this. A frequency-domain Wiener inverse was
	// the first attempt and it has a failure mode worth remembering: where
	// the cable has thrown the signal away, the Wiener solution degenerates
	// into a MATCHED FILTER, which smooths. So the equaliser at maximum made
	// a hundred-metre run softer than leaving it alone -- mathematically the
	// optimal thing to do with a lost signal, and the exact opposite of what
	// an equaliser is. `fwtest --eq` asserts against it directly now: the
	// cascade must never be softer than the cable on its own.
	//
	// The normal equations are R e = b with R the autocorrelation of h -- a
	// symmetric Toeplitz matrix -- and b the single value h[0] in its first
	// row, because the target is an impulse at the origin.
	//---------------------------------------------------------------------
	std::vector< double > autocorrelation( kTaps, 0.0 );
	for( int lag = 0; lag < kTaps; ++lag )
	{
		double sum = 0.0;
		for( int n = 0; n + lag < kDesignLength; ++n )
			sum += h[ n ] * h[ n + lag ];
		autocorrelation[ lag ] = sum;
	}

	std::vector< double > matrix( static_cast< size_t >( kTaps ) * kTaps, 0.0 );
	for( int i = 0; i < kTaps; ++i )
		for( int j = 0; j < kTaps; ++j )
			matrix[ static_cast< size_t >( i ) * kTaps + j ] =
			    autocorrelation[ std::abs( i - j ) ] + ( i == j ? kNoiseFloor : 0.0 );

	//Cholesky. The matrix is an autocorrelation plus a positive diagonal, so
	//it is positive definite by construction and this cannot fail -- but the
	//guard stays, because "cannot fail" plus a NaN in a shader uniform is a
	//black frame in somebody's show.
	std::vector< double > lower( static_cast< size_t >( kTaps ) * kTaps, 0.0 );
	for( int i = 0; i < kTaps; ++i )
	{
		for( int j = 0; j <= i; ++j )
		{
			double sum = matrix[ static_cast< size_t >( i ) * kTaps + j ];
			for( int k = 0; k < j; ++k )
				sum -= lower[ static_cast< size_t >( i ) * kTaps + k ] * lower[ static_cast< size_t >( j ) * kTaps + k ];

			if( i == j )
			{
				if( sum <= 0.0 )
					return;//leave the equaliser flat rather than hand out a NaN
				lower[ static_cast< size_t >( i ) * kTaps + j ] = std::sqrt( sum );
			}
			else
			{
				lower[ static_cast< size_t >( i ) * kTaps + j ] =
				    sum / lower[ static_cast< size_t >( j ) * kTaps + j ];
			}
		}
	}

	std::vector< double > forward( kTaps, 0.0 );
	for( int i = 0; i < kTaps; ++i )
	{
		double sum = ( i == 0 ) ? h[ 0 ] : 0.0;
		for( int k = 0; k < i; ++k )
			sum -= lower[ static_cast< size_t >( i ) * kTaps + k ] * forward[ k ];
		forward[ i ] = sum / lower[ static_cast< size_t >( i ) * kTaps + i ];
	}

	std::vector< double > taps( kTaps, 0.0 );
	for( int i = kTaps - 1; i >= 0; --i )
	{
		double sum = forward[ i ];
		for( int k = i + 1; k < kTaps; ++k )
			sum -= lower[ static_cast< size_t >( k ) * kTaps + i ] * taps[ k ];
		taps[ i ] = sum / lower[ static_cast< size_t >( i ) * kTaps + i ];
	}

	//Unit DC gain. An equaliser changes the balance of a picture, never its
	//brightness -- and the least-squares answer is a per cent or two off,
	//because it was asked to match an impulse and not to conserve light.
	double sum = 0.0;
	for( double t : taps )
		sum += t;
	if( std::fabs( sum ) < 1.0e-9 )
		return;
	for( double& t : taps )
		t /= sum;

	//Blend from flat. Above 1 the operator has dialled in more cable than is
	//actually out there, and gets the over-equalised look: a bright outline
	//on the trailing side of every edge.
	for( int n = 0; n < kTaps; ++n )
	{
		const double flat = ( n == 0 ) ? 1.0 : 0.0;
		out[ n ]          = static_cast< float >( flat + static_cast< double >( amount ) * ( taps[ n ] - flat ) );
	}
}

//---------------------------------------------------------------------------
void convolve( const float a[ kTaps ], const float b[ kTaps ], float out[ kTaps ] )
{
	float scratch[ kTaps ] = {};
	for( int n = 0; n < kTaps; ++n )
	{
		float sum = 0.0f;
		for( int k = 0; k <= n; ++k )
			sum += a[ k ] * b[ n - k ];
		scratch[ n ] = sum;
	}

	for( int n = 0; n < kTaps; ++n )
		out[ n ] = scratch[ n ];
}

//---------------------------------------------------------------------------
float alphaFor( const CableSpec& spec, float metres, float pixelClockHz )
{
	if( metres <= 0.0f || pixelClockHz <= 0.0f )
		return 0.0f;

	//dB/100 m/sqrt(MHz) -> the sqrt(seconds) constant of exp(-alpha*sqrt(pi*f)).
	//The 8.686 is nepers to dB and the 1000 is sqrt(MHz) to sqrt(Hz).
	constexpr float kToNepers = 1.0f / ( 100.0f * 1000.0f * 8.685889638f * 1.772453851f );

	const float alphaSeconds = spec.lossDbPer100m * metres * kToNepers;

	//Into pixels. alpha divides a sqrt(time), so it scales by sqrt(clock) --
	//which is why doubling the pixel clock costs only 41% more softness, the
	//same square-root that makes coax useful in the first place.
	return alphaSeconds * std::sqrt( pixelClockHz );
}

float transitPixels( const CableSpec& spec, float metres, float pixelClockHz )
{
	constexpr float c = 299792458.0f;
	if( metres <= 0.0f || spec.velocity <= 0.0f )
		return 0.0f;

	return metres / ( spec.velocity * c ) * pixelClockHz;
}

float reflection( float impedanceOhms )
{
	constexpr float z0 = 75.0f;
	return ( impedanceOhms - z0 ) / ( impedanceOhms + z0 );
}

float bandwidthCyclesPerPixel( float alpha )
{
	if( alpha <= 0.0f )
		return 1.0f;

	//|H| = exp(-alpha*sqrt(pi*f)) = 1/sqrt(2) at alpha*sqrt(pi*f) = 0.34657.
	const float root = 0.34657359f / alpha;
	return root * root / static_cast< float >( kPi );
}

} // namespace fivewire
