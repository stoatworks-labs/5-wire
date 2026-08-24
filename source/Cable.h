#pragma once

/**
    The cable, as a filter.

    Everything 5-wire renders comes out of this file. A long RGBHV run does not
    apply five separate effects to a picture -- it applies ONE linear response
    per conductor, and the things everybody recognises are consequences of it:

      soft, smeared picture        the response's head
      streaking after bright areas the same response's tail, hundreds of
                                   pixels long and a hundredth as tall
      a displaced repeat           the same response, arriving twice, because
                                   a mismatched end sends some of it back
      coloured fringes on edges    the same response arriving at three
                                   different times on three conductors
      a haze of noise              the only thing here that is NOT the cable:
                                   it is picked up along the way, which is why
                                   the equaliser at the far end lifts it

    ------------------------------------------------------------------ units

    The cable works in TIME and the picture works in PIXELS, and the pixel
    clock is the only thing that connects them. That is not a detail to hide:
    the same 30 m of coax that is invisible on 640x480 is a visible ghost on
    1600x1200, because the picture is clocked four times faster and the same
    nanosecond is four times as many pixels. So every quantity in here is
    converted through `pixelClockHz` at the boundary, and the shaders never
    see a second.

    -------------------------------------------------------- the loss itself

    Coax loss is a skin-effect loss: the attenuation goes as the SQUARE ROOT of
    frequency, not linearly, which is why the datasheets quote dB/100 m at a
    frequency and why doubling the bandwidth costs only 41% more loss. In the
    time domain a sqrt(f) loss has a closed-form step response:

        S(t) = erfc( alpha / (2 * sqrt(t)) )

    -- the Heaviside cable equation, and the reason the impulse response here
    is not a Gaussian blur, not a box, and above all NOT SYMMETRIC. It is
    strictly causal with a long t^-3/2 tail, so a bright edge smears to the
    RIGHT and only to the right. A symmetric blur is the single commonest way
    to get this artefact wrong, and it reads instantly as "blurred" rather than
    as "long cable".

    That one function gives the head and the tail together. The head is
    sampled into `kTaps` pixel bins and convolved directly; the tail is far too
    long for that (a hundred metres of thin coax is still ringing five hundred
    pixels later) so it is integrated into `kWideTaps` box weights per level
    and read from horizontally-reduced buffers. Same curve, two budgets.
*/

namespace fivewire
{
//---------------------------------------------------------------------------
// What kind of cable is on the floor.
//
// The numbers are the manufacturers' own: attenuation quoted as dB per 100 m
// at a frequency, converted to the sqrt(f) constant that produced it. They are
// what makes the types feel different from each other, and none of them is a
// taste decision.
//---------------------------------------------------------------------------
struct CableSpec
{
	const char* name;

	/// dB of loss per 100 m per sqrt(MHz). RG-59's 1.1 comes from the usual
	/// 11 dB/100 m at 100 MHz; the thin bundle inside a moulded VGA lead is
	/// three times worse because the centre conductor is 28 AWG.
	float lossDbPer100m;

	/// Propagation velocity as a fraction of c. This sets where the ghost
	/// lands, and it is why a ghost off a CAT5 run sits slightly closer than
	/// the same length of foam coax.
	float velocity;

	/// Spread of propagation delay between the three video conductors, ns per
	/// 100 m. Near zero for cut-to-length coax; tens of nanoseconds for UTP,
	/// where the four pairs are deliberately twisted at different rates to
	/// stop them coupling and therefore have genuinely different lengths.
	float skewNsPer100m;

	/// Which way that spread goes, per conductor. Sums to zero, so Skew moves
	/// the channels apart without moving the picture.
	float skewPattern[ 3 ];

	/// Coupling between conductors per 100 m, at Crosstalk = 1.
	///
	/// Crosstalk is a DERIVATIVE effect -- mutual inductance couples a
	/// neighbour's rate of change, not its level -- so it shows up as coloured
	/// outlines on edges and never as a tint over flat colour, which is the
	/// difference between crosstalk and a bad white balance.
	///
	/// The unit is against the received edge's own slope PER PIXEL, which is
	/// why these are numbers around one rather than the fractions of a per
	/// cent a cable datasheet quotes for NEXT. It also has a consequence worth
	/// knowing: the softer the cable has made the picture, the less crosstalk
	/// it produces, because there is less rate of change left to couple. A
	/// long run's coloured outlines come from its length; a short run's come
	/// from its edges.
	float crosstalkPer100m;

	/// How well the run keeps the room out, 1 = solid copper braid. Scales
	/// mains hum and RF ingress together, because they arrive by the same
	/// route and no real cable is good at one and bad at the other.
	float shielding;
};

int cableCount();
const CableSpec& cable( int index );

//---------------------------------------------------------------------------
// The kernel budget.
//
// kTaps is the head: 64 pixels, convolved tap by tap. Everything past that is
// the tail, integrated into boxes read from reduced buffers -- 8 boxes of 8
// pixels covering 64..127, then 8 boxes of 64 covering 128..639.
//
// Past 640 pixels the response is truncated and the whole kernel renormalised
// to unit sum. Renormalising rather than dropping the remainder is the choice
// that matters: unit DC gain is the one property a passive cable is
// guaranteed to have, and a kernel that sums to 0.98 dims the picture by 2%
// while claiming to be a cable.
//---------------------------------------------------------------------------
inline constexpr int kTaps       = 64;
inline constexpr int kWideTaps   = 8;
inline constexpr int kWideLevels = 2;
inline constexpr int kWideStride[ kWideLevels ] = { 8, 64 };
inline constexpr int kWideStart[ kWideLevels ]  = { 64, 128 };

struct Kernel
{
	float tap[ kTaps ]                     = {};
	float wide[ kWideLevels ][ kWideTaps ] = {};

	/// How much of the response the head holds. Below about 0.98 the tail is
	/// worth two reduction passes; above it, they are skipped.
	float headSum = 1.0f;
};

/// The cable's own impulse response, in pixel bins.
///
/// `alpha` is the sqrt(f) loss constant expressed in sqrt(pixels) -- see
/// `alphaFor()`. Zero gives the identity: tap[0] = 1 and nothing else.
Kernel lossKernel( float alpha );

/// The equaliser: a regularised inverse of `lossKernel(alpha)`, blended by
/// `amount`, as a compact FIR.
///
/// Designed by regularised least squares against a noise floor, which is what
/// an equaliser physically is and what physically limits it. A cable that has
/// thrown away 40 dB at the pixel rate has put that detail below the receiver's
/// own noise, and no amount of gain at the far end brings it back; it brings
/// the NOISE back. That trade is the regulariser, and it is why Cable EQ at
/// maximum on a hundred-metre run gives a picture several times sharper and
/// grainy rather than one perfectly restored.
///
/// It is NOT an exact inverse. One exists and is a line of recursion, and past
/// about alpha 1.2 its coefficients run to 1e11 -- because 64 taps genuinely
/// cannot undo a response six hundred pixels long. See the note in the
/// implementation, which also records the frequency-domain version this
/// replaced and the reason it was wrong.
///
/// amount 0 = flat, 1 = set for exactly this cable, above 1 = over-equalised,
/// which rings and puts a bright outline on the trailing side of every edge.
/// A real cable EQ is calibrated in METRES for the same reason, which is why
/// the control that feeds `alpha` here is a length and not a frequency.
void equaliserKernel( float alpha, float amount, float out[ kTaps ] );

/// out = a convolved with b, truncated back to kTaps. Truncation is safe here
/// because both inputs are causal and concentrated at the head.
void convolve( const float a[ kTaps ], const float b[ kTaps ], float out[ kTaps ] );

//---------------------------------------------------------------------------
// The conversions. Each one is a line of physics and the only place it is
// written down.
//---------------------------------------------------------------------------

/// The sqrt(f) loss constant in sqrt(pixels), from a cable, a length and a
/// pixel clock.
///
/// A cable losing `d` dB per 100 m per sqrt(MHz) over `metres` has
/// |H(f)| = exp(-k*sqrt(pi*f)) with k = d*metres / (100 * 1000 * 8.686 * sqrt(pi))
/// in sqrt(seconds); scaling into pixels is a multiply by sqrt(clock).
float alphaFor( const CableSpec& spec, float metres, float pixelClockHz );

/// One-way propagation delay, in pixels. A ghost is two of these.
float transitPixels( const CableSpec& spec, float metres, float pixelClockHz );

/// Reflection coefficient of a termination, against the 75 ohm the line was
/// built for. Negative below 75 (a doubled-up terminator: the repeat is DARK),
/// positive above (a monitor with no terminator at all: the repeat is BRIGHT).
/// Exactly zero at 75, which is the point of terminating anything.
float reflection( float impedanceOhms );

/// -3 dB bandwidth of the cable, in cycles per pixel. Only used for reporting
/// and by the harness -- nothing renders from it -- but it is the number that
/// says whether a run is usable: above 0.5 the cable outruns the pixel clock
/// and the picture is sharp.
float bandwidthCyclesPerPixel( float alpha );

} // namespace fivewire
