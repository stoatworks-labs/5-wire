# 5-wire — for another LLM, or a newcomer

Read this before changing the cable model, the order of the passes, or where
anything is added to the signal. `CLAUDE.md` is the short command reference;
this file is the *why*.

## The one idea

**The cable is a filter, the filter is one-sided, and everything else is a
consequence of where in the chain a thing joins the signal.**

Coax loses the **square root** of frequency — skin effect, which is why the
datasheets quote dB/100 m at a frequency and why doubling the bandwidth costs
only 41% more loss rather than 100%. A square-root loss has a closed-form step
response:

    S(t) = erfc( alpha / (2 * sqrt(t)) )

That is the Heaviside cable equation, and three things about it are the whole
plugin:

| It is | Which means |
| --- | --- |
| **strictly causal** | a bright edge smears to the RIGHT and only to the right |
| **heavy-tailed** (t^-3/2) | the same curve is still going hundreds of pixels later, which is what streaking IS |
| **one parameter** | `alpha`, from the cable, the length and the pixel clock — and nothing else |

A symmetric blur is the single commonest way to get this artefact wrong. It
reads instantly as "blurred" rather than as "long cable", and no amount of
tuning recovers it, because the direction is the information.

Everything else falls out of the chain rather than having a control of its own:

| Symptom | What causes it | Where it lives |
| --- | --- | --- |
| A displaced repeat | both ends are the wrong impedance | Compose |
| Repeats getting softer the further out they land | each bounce is two more trips down the cable | `ghostBlurPx` |
| Coloured fringes on vertical edges | the three conductors are different lengths | Line, `SkewPx` |
| Coloured outlines on edges only | conductors couple each other's RATE OF CHANGE | Compose, `Crosstalk` |
| Hum bars that bend the lines they cross | hum is on the sync conductor too | Compose |
| Jitter, then a rolling frame | sync came down the same cable and the receiver ran out of margin | Compose |
| A dark trail after a bright area | the clamp failed and the black level is following the picture | Receive |

## The argument the plugin exists to make

**Pre-Emphasis and Cable EQ are the same filter, applied at opposite ends, and
they are not the same control.**

They both invert the cable. They both make the picture sharp again. But the
noise, the reflections and the crosstalk join the signal **at the far end**, so:

- **Cable EQ**, at the receiver, is *after* them. It lifts them with the
  picture. A hundred-metre run equalised at the display is sharp and grainy,
  with the ghost lifted too.
- **Pre-Emphasis**, at the amplifier, is *before* them. It lifts nothing except
  the picture — and costs headroom instead, because it overshoots on every edge
  and the overshoot meets the rail.

That is why a real distribution amplifier offers both, and it is why the
`Headroom` control exists. Everything about the pass order is arranged so this
comes out on its own rather than being asserted.

## Load-bearing invariants

**The amplifier and the cable are two passes, not one.** Two linear filters in
series are one filter and folding them would save a full-resolution pass. They
stay separate because the amplifier has a **rail**, a rail is not linear, and
the overshoot has to meet it *before* the cable smears it. Fold them and
`Headroom` becomes a control that does almost nothing, because by then the peak
it was meant to catch has been spread over sixty pixels.

**The kernel sums to one, always.** `lossKernel` renormalises after truncating
at 640 pixels. Unit DC gain is the one property a passive cable is guaranteed
to have, and a kernel that sums to 0.98 dims the picture by two per cent while
claiming to be a cable. The cost of that choice is that past about `alpha` 1.25
the truncated tail is given back slightly too early, which lifts the low end by
a per cent or two — visible in `fwtest --kernel`'s right-hand column, and
stated there rather than hidden in a tolerance.

**The response is sampled by INTEGRATING over each pixel bin**, not by point
sampling. The impulse response goes as t^-3/2 and is infinite at the origin, so
a point sample near zero means nothing while the integral over the bin is
exact. The consequence is that near Nyquist the taps roll off slightly faster
than the continuous law — that extra roll-off belongs to the pixel, not to the
cable, and `fwtest --kernel` asserts the law only below 0.15 cycles/pixel for
exactly that reason.

**The equaliser is designed by regularised least squares, and this matters
twice.**

- It is **not an exact inverse**. One exists and is a line of recursion, and
  past about `alpha` 1.2 its coefficients run to 1e11 — because 64 taps
  genuinely cannot undo a response six hundred pixels long. The recursion does
  not fail; it returns enormous numbers and the picture becomes noise.
- It is **not a Wiener inverse either**, which is what it was first written as.
  A Wiener inverse degenerates into a matched filter where the cable has thrown
  the signal away, and a matched filter **smooths** — so at a hundred metres,
  Cable EQ at maximum made the picture *softer* than leaving it alone. That is
  the mathematically optimal thing to do with a lost signal and the exact
  opposite of what an equaliser is. `fwtest --eq` now asserts directly against
  it: the cascade may never be softer than the cable on its own.

**The equaliser is calibrated in METRES.** `EQ Length` is a length, not a
frequency, exactly like the knob on a real cable equaliser — which is what
makes "set for the wrong length" an ordinary, recognisable mistake rather than
an abstract one.

**Sync barely notices the cable.** An H sync pulse is a few microseconds wide —
tens of kilohertz — against a pixel clock of a hundred megahertz. A run that has
thrown away three quarters of the picture's bandwidth has taken almost nothing
off the sync *amplitude*; what it has taken is the *edge*, so the slicer's
timing wanders. Length therefore buys **jitter**, and loss of lock comes from an
amplifier that was not driving enough sync in the first place. This was first
written as `alpha * 0.9`, which put every hundred-metre run into a rolling
frame. It looked spectacular and it is not what happens: a long run goes soft
and stays locked.

**Rolling moves the picture, not the interference.** The hum bar and the
herringbone are generated at the un-rolled position, so a frame that has lost
lock runs upwards *through* a bar that stays where it is. Roll them together and
it reads as an effect drawing a roll rather than as a receiver losing one.

**The frame period is measured, not assumed.** It decides whether the hum bar
stands still or crawls: 50 Hz on a 50 Hz frame rate puts every line at the same
phase and the bar does not move, and one hertz out it creeps. Assume 60 and
that behaviour is simply wrong at every other rate.

**The reduced buffers are horizontal only.** The cable's response is a fact
about time, a scan line is the only axis that carries time, and reducing
vertically would invent a coupling between scan lines that no cable has.

**Everything is premultiplied.** Gain, hum and the black level scale or offset
colour alone. Noise and ingress are additionally scaled by alpha — not physics,
but the compositing rule: a premultiplied pixel may not carry colour outside its
own coverage, and noise glowing in the empty half of a keyed layer is what
forgetting it looks like.

**The vertex shader passes UVs through unscaled.** Three of the five passes read
this plugin's own buffers, where MaxUV is 1 and the host's is not, and every
fetch in the other two is an offset measured in *pixels of the picture*. Folding
MaxUV into the varying puts every offset in the wrong units in exactly the
passes where the offsets are the effect.

## Where things live

| File | What it is |
| --- | --- |
| `source/Cable.{h,cpp}` | the five cable types, the loss kernel, the equaliser design. No GL, no state. |
| `source/Controls.{h,cpp}` | 0..1 host values → metres, hertz and ohms. `drive()` is the only place any of it is written down. |
| `source/shaders/Head.cpp` | the amplifier: gain, channel drives, pre-emphasis, the rail. |
| `source/shaders/Line.cpp` | the cable, and the 8:1 reduction used twice. |
| `source/shaders/Compose.cpp` | the far end: the tail, the repeats, crosstalk, pickup, the receiver's timing. |
| `source/shaders/Receive.cpp` | equaliser, clamp, sampler. |
| `source/FiveWire.{h,cpp}` | FFGL host glue, the clock, the parameters, the presets. |
| `tools/fwtest` | the offline harness and the five checks. |

## Traps

- **Resolume does NOT consume `FF_EVENT_FLAG_VALUE`.** It keeps pushing the
  values it still believes in, those arrive as ordinary parameter writes with a
  changed value, and a copy-based preset applier reads its own host's echo as an
  operator edit and drops back to Custom before a frame is rendered.
  `hostIsRestatingItself()` is the fix and `fwtest --presets` is the test; it
  fails on the naive version in precisely the "ignores events" column.
  `seedHostValues()` must run **before** `applyPreset` can, or the preset's own
  values become the host's opening position and the bug comes straight back.
- **A GLSL uniform name that does not match the C++ is silently ignored.**
  `glGetUniformLocation` returns -1 and `glUniform(-1)` is a documented no-op,
  so a control can be stone dead while everything compiles and renders. Only
  `tools/sweep.py` catches it.
- **`FFGLShader::Set` has no array overload**, so the kernels go up through
  `glUniform1fv` by hand in `setArray()`. A wrong name there gives a kernel of
  zeros — an entirely black picture, which is at least unmissable.
- **`SetTextParameter` must return FF_SUCCESS for the About block.** The SDK's
  `instantiateGL` sets every parameter's default on a fresh instance and deletes
  the instance if any set fails, and the base class's version is a stub
  returning FF_FAIL. Omit the override and **no real host can instantiate the
  plugin at all** — while every harness here stays happy, because they drive the
  class directly and never go through `plugMain`.
- **`ScopedFBOBinding` restores the framebuffer and not the viewport.** Capture
  the host viewport at the top of `ProcessOpenGL`.
- **Allocating an FBO unbinds your input texture.** Every `Ensure()` happens
  before any texture binding for that reason. The symptom is correct on every
  frame *except* the one that allocates.
- **`ffglex::FFGLFBO::Release()` leaks the colour texture** (SDK b1afaf9): it
  tests `depthBufferID` a second time where it meant `colorTextureID`.
  `PassBuffer::Destroy()` deletes it first.
- **Resolume sends `SetTime` in milliseconds.** The unit is decided from the
  first plausible frame delta and nothing consumes `hostTime` raw. Getting it
  wrong is three orders of magnitude on the hum crawl and the roll rate.
- **`layout` is a GLSL reserved word**, as are `flat`, `filter`, `input`,
  `output`, `sample`, `common`. A shader that fails to compile surfaces at
  runtime as "the effect does nothing", with the reason only in
  `~/Library/Logs/5-wire/`.
- **The reduced buffers are read on frames where nothing wrote them.** They are
  allocated always and rendered only when the tail, the clamp or sync-on-green
  needs them; on the frames in between their weights are zero. That is why
  `PassBuffer::Ensure` clears a newly allocated buffer — undefined contents
  there is whatever texture memory the driver handed back.
- **`Skew` is genuinely dead on coax**, which is what coax is for. `sweep.py`
  needs a CONTEXT entry putting a CAT5 balun on the floor, and several other
  controls are properties of something else being switched on.
- **Never let `tools/sweep.py` touch the About block.** Those parameters are
  buttons that open a web browser.

## The one deliberate approximation

A repeat has been down the cable two more times than the picture for every
bounce it has made, so it should be convolved with the loss kernel three, five
or seven times over. It is not: it is sampled from the cable pass through a
five-tap box whose width is where the *extra* response reaches half height
(`1.1 * alpha^2`). The direction and the amount are right and the shape is not
— a box where there should be another one-sided tail. It is the only place in
the plugin where a symptom is drawn rather than derived, and it costs five
fetches per bounce instead of sixty-four.

## What is genuinely verified, and what is assumed

**Verified, by `tools/verify.sh`:**

- The cable's response has unit gain at DC to 1e-5, is strictly one-sided and
  never negative, never rises with frequency, and its measured -3 dB point
  tracks the analytic square-root law across the whole usable range.
- The equaliser has unit DC gain, is exactly flat at amount zero, gets
  monotonically worse as the run gets longer, corrects a recoverable cable to
  within 4%, and **never** makes the cascade softer than the cable alone.
- **End to end**: one bright column through the whole chain comes out equal to
  `Cable.cpp`'s kernel to within a thousandth of a level, on four cables, and
  delivers 99% of what went in — which is the check that the taps the GPU is
  convolving with are the taps that were computed.
- The repeats land within two pixels of twice the transit time at three
  lengths, and a properly back-matched amplifier produces none at all against an
  open far end.
- All ten factory presets survive all three host behaviours, including the one
  Resolume actually has.
- All 30 parameters change the picture.
- The bundle exports `plugMain`, carries `5W01` and nobody else's id, names its
  own binary in its plist, and ad-hoc signs.
- The macOS build is universal, by `lipo`.

**Assumed, and not yet checked:**

- **It has never been loaded into Resolume.** It has never been loaded into any
  FFGL host. Everything above comes from a harness that drives the plugin class
  directly, which means the parameter declarations, the group layout, the About
  block and the event handling are all unproven in a real host — the exact class
  of thing `SetTextParameter` is a trap about.
- **Nothing has been built on Windows or Linux.** The macOS build IS universal
  and checked with `lipo`; that one is verified.
- **There is no OpenFX build.** The model is deliberately in `Cable.cpp` and
  `Controls.cpp` with no GL in either, so an OFX target can link them straight
  from source and only the per-pixel half would need mirroring.
- **The cable numbers are the manufacturers'; the feel constants are not.** The
  loss figures, velocity factors and CAT5 skew come from datasheets. The
  crosstalk coefficients, the pickup model, the sync-margin thresholds and the
  jitter scaling are calibrated by eye to be usable across a slider, and no part
  of them has been measured against a real run.
- **No real long run has been photographed and compared against this.** The
  model is right; whether the *amounts* match a particular cable on a particular
  floor is not something anything here has tested.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
