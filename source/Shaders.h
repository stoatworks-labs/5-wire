#pragma once

/**
    The GLSL for each pass.

    The chain is the signal path, in the order the signal actually travels, and
    that order is the model. Move a stage and the plugin stops being true:

      Line     the amplifier and the cable. Pre-emphasis and the cable's own
               loss are ONE convolution, because two linear filters in series
               are one filter, and per-conductor skew is a fractional offset
               inside it. Everything here happens BEFORE anything is added to
               the signal, which is the entire reason pre-emphasis is worth
               having.

      Wide8    two horizontal reductions of that result, 8:1 and then 64:1.
      Wide64   The cable's response has a tail hundreds of pixels long -- it
               is what streaking IS -- and these are how it gets read without
               a six-hundred-tap convolution. Skipped on short runs.

      Compose  the far end of the cable: the reflections arriving late, the
               conductors coupling into each other, and everything the room
               has put on top. Also where the receiver's timing lives, because
               sync came down the same cable and a receiver that cannot find
               it puts the line in the wrong place.

      Receive  the equaliser, the clamp and the ADC. AFTER Compose, and that
               is the point of the whole plugin: the equaliser at this end
               lifts the noise and the ghosts along with the picture, and the
               identical filter at the other end does not.

    -------------------------------------------------------------- premultiply

    Everything is premultiplied. Anything that changes how much light there is
    (gain, hum, the black level) scales or offsets colour alone; anything added
    that a cable would carry (noise, ingress) is scaled by alpha so it stays
    inside the picture's own coverage rather than glowing in the empty half of
    a keyed layer.

    -------------------------------------------------------------------- MaxUV

    The vertex shader passes texture coordinates through UNSCALED and every
    fetch applies MaxUV itself. Three of the five passes read this plugin's own
    buffers, where MaxUV is 1 and the host's is not -- and every fetch in the
    other two is a horizontal offset measured in PIXELS OF THE PICTURE, which
    is a different thing from a fraction of a padded host texture. Folding the
    scale into the varying would leave every offset in the wrong units in
    exactly the passes where the offsets are the effect.
*/

namespace fivewire::shaders
{
/// Draws the screen quad. `uv` is 0..1 across the picture.
extern const char* const kVertex;

/// The amplifier: gain, the channel drives, pre-emphasis and the rail.
extern const char* const kHeadFragment;

/// The cable: one convolution, three conductors.
extern const char* const kLineFragment;

/// An 8:1 horizontal box. Used twice, at two scales, which is the only reason
/// it is a separate string rather than part of the compose pass.
extern const char* const kWideFragment;

/// The far end: reflections, crosstalk, pickup, and the receiver's timing.
extern const char* const kComposeFragment;

/// The equaliser, the clamp and the sampler.
extern const char* const kReceiveFragment;

/**
    There is no GLSL twin of the model here, and that is deliberate.

    Every other plugin in the fleet with a physical model has one written twice
    -- once in C++ for the harness and once in GLSL for the picture -- and a
    test whose whole job is to prove the two have not drifted. This one has the
    model in C++ only: `Cable.cpp` computes the taps, the plugin uploads them,
    and the shaders are convolutions that do not know what they are convolving
    with. So there is nothing to drift, and the check that replaces it is
    `fwtest --impulse`: one bright column through the whole chain, and the row
    that comes out compared against Cable.cpp tap by tap. That catches the
    thing which CAN still go wrong here, and which a drift test never could --
    an upload that silently did not happen, leaving the GPU convolving with
    whatever was in the uniform before.
*/

} // namespace fivewire::shaders
