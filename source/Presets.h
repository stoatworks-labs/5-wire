#pragma once

/**
    Factory presets, in the host's own 0..1 space.

    One table, read by every build, so two builds of 5-wire cannot disagree
    about what "Long Haul" means. Element 0 of the dropdown is always Custom
    and is NOT in this table.

    What the presets deliberately do not cover:

      Pixel Clock   the raster the operator is actually running, not a look. A
                    preset that moved it would silently retune every length,
                    ghost distance and skew in the composition.
      Mains         50 or 60 Hz is a fact about the country the show is in.

    Every preset here is a real situation somebody has stood in a venue and
    looked at, which is why the pairs matter: Long Haul and Equalised are the
    same cable with the equaliser off and on, and the whole point is that the
    second one is sharp AND grainy.
*/

namespace fivewire::presets
{
/// The parameters a preset covers, in the order the table lists them. The
/// FFGL build binds these to its own IDs in one array; nothing here knows
/// what an FFGL parameter is.
enum Param
{
	P_CABLE_TYPE,
	P_LENGTH,
	P_TERMINATION,
	P_GHOSTING,
	P_BOUNCES,
	P_SKEW,
	P_CROSSTALK,
	P_SCREENING,

	P_NOISE,
	P_HUM,
	P_INGRESS,
	P_INGRESS_PITCH,

	P_SYNC_LEVEL,
	P_SYNC_ON_GREEN,
	P_JITTER,

	P_GAIN,
	P_RED,
	P_GREEN,
	P_BLUE,
	P_PRE_EMPHASIS,
	P_EQ_LENGTH,
	P_HEADROOM,

	P_CABLE_EQ,
	P_OUTPUT_GAIN,
	P_BLACK,
	P_RESTORE,
	P_SAMPLE_PHASE,

	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

//                        type  len   term  ghost bnce  skew  xtlk  scrn | noise hum   ingr  pitch | sync  sog   jit  | gain  red   grn   blu   pre   eqlen head | eq    out   blk   rest  phase
inline constexpr Preset kPresets[] = {
	//A proper installation: five coaxes, terminated, and near enough
	//transparent. The preset that proves the plugin is not simply a blur.
	{ "House Run",      { 0.00f, 0.45f, 0.50f, 0.20f, 0.00f, 0.20f, 0.15f, 0.60f, 0.08f, 0.05f, 0.03f, 0.35f, 0.85f, 0.00f, 0.05f, 0.50f, 0.50f, 0.50f, 0.50f, 0.00f, 0.45f, 0.60f, 0.00f, 0.50f, 0.50f, 0.95f, 0.00f } },

	//A hundred metres of thin coax with nothing done about it. Soft, and
	//streaking behind everything bright.
	{ "Long Haul",      { 2.00f, 0.82f, 0.55f, 0.30f, 0.00f, 0.30f, 0.35f, 0.50f, 0.15f, 0.20f, 0.10f, 0.30f, 0.70f, 0.00f, 0.25f, 0.50f, 0.50f, 0.50f, 0.50f, 0.00f, 0.82f, 0.50f, 0.00f, 0.50f, 0.50f, 0.55f, 0.00f } },

	//The same run with the equaliser wound up. Sharp again -- and the noise,
	//the ghost and the crosstalk came up with it, because they joined the
	//signal after the loss and before the equaliser.
	{ "Equalised",      { 2.00f, 0.82f, 0.55f, 0.30f, 0.00f, 0.30f, 0.35f, 0.50f, 0.15f, 0.20f, 0.10f, 0.30f, 0.70f, 0.00f, 0.25f, 0.50f, 0.50f, 0.50f, 0.50f, 0.00f, 0.82f, 0.50f, 0.85f, 0.50f, 0.50f, 0.55f, 0.00f } },

	//And the same run pre-emphasised at the amplifier instead. Sharp and
	//clean, at the cost of headroom -- which is what the clipped whites are.
	{ "Pre-Emphasised", { 2.00f, 0.82f, 0.55f, 0.30f, 0.00f, 0.30f, 0.35f, 0.50f, 0.15f, 0.20f, 0.10f, 0.30f, 0.70f, 0.00f, 0.25f, 0.50f, 0.50f, 0.50f, 0.50f, 0.80f, 0.82f, 0.20f, 0.00f, 0.50f, 0.50f, 0.55f, 0.00f } },

	//Nobody terminated the far end and the amplifier has no series resistor.
	//A ghost train, marching to the right.
	{ "Unterminated",   { 0.00f, 0.60f, 0.86f, 0.55f, 2.00f, 0.20f, 0.20f, 0.60f, 0.10f, 0.08f, 0.05f, 0.35f, 0.80f, 0.00f, 0.10f, 0.50f, 0.50f, 0.50f, 0.50f, 0.00f, 0.60f, 0.50f, 0.00f, 0.50f, 0.50f, 0.90f, 0.00f } },

	//The lead out of the bottom of the flight case, run twenty-five metres
	//across a floor full of dimmer packs.
	{ "Skip Lead",      { 1.00f, 0.42f, 0.72f, 0.45f, 1.00f, 0.40f, 0.55f, 0.30f, 0.35f, 0.65f, 0.45f, 0.28f, 0.65f, 0.00f, 0.40f, 0.50f, 0.50f, 0.48f, 0.46f, 0.00f, 0.42f, 0.50f, 0.25f, 0.52f, 0.50f, 0.70f, 0.35f } },

	//Passive baluns over structured cabling. The pairs are different lengths
	//by design, so every vertical edge grows a colour fringe.
	{ "CAT5 Extender",  { 3.00f, 0.70f, 0.60f, 0.35f, 0.00f, 0.85f, 0.60f, 0.35f, 0.25f, 0.30f, 0.35f, 0.40f, 0.60f, 1.00f, 0.35f, 0.50f, 0.50f, 0.50f, 0.50f, 0.00f, 0.70f, 0.50f, 0.45f, 0.50f, 0.50f, 0.75f, 0.00f } },

	//Sync is on its last legs. Lines land in the wrong place, and the frame
	//has started to run.
	{ "Losing Sync",    { 1.00f, 0.62f, 0.65f, 0.40f, 1.00f, 0.45f, 0.50f, 0.30f, 0.30f, 0.45f, 0.25f, 0.32f, 0.20f, 1.00f, 0.85f, 0.50f, 0.50f, 0.50f, 0.50f, 0.00f, 0.62f, 0.50f, 0.30f, 0.50f, 0.50f, 0.65f, 0.20f } },

	//An amplifier that has been in the rack since the last century: the
	//clamp has given up, the channels have drifted apart and the phase is
	//half a pixel out.
	{ "Tired Amp",      { 1.00f, 0.50f, 0.68f, 0.50f, 1.00f, 0.40f, 0.45f, 0.35f, 0.30f, 0.40f, 0.20f, 0.35f, 0.55f, 0.00f, 0.45f, 0.46f, 0.545f, 0.50f, 0.455f, 0.35f, 0.75f, 0.15f, 0.55f, 0.58f, 0.44f, 0.15f, 0.50f } },

	//Everything at once, for the shot where the picture is meant to be
	//falling apart rather than merely tired.
	{ "Dead Run",       { 4.00f, 0.95f, 0.86f, 0.60f, 2.00f, 0.90f, 0.90f, 0.15f, 0.35f, 0.60f, 0.50f, 0.42f, 0.15f, 1.00f, 0.95f, 0.50f, 0.52f, 0.48f, 0.55f, 0.00f, 0.55f, 0.35f, 0.50f, 0.50f, 0.50f, 0.05f, 0.45f } },
};

inline constexpr int kCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace fivewire::presets
