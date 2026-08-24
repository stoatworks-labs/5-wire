# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*5-wire — long VGA/RGBHV cable runs as an FFGL effect; RELEASED v0.1.0 2026-08-24, all six homes live, never loaded into Resolume*

**5-wire** — a long run of VGA or RGBHV and the amplifier at each end, as an
FFGL 2.1 effect for Resolume (ID `5W01`, display name "5-wire").
`~/projects/resolume/5-wire`, C++17 + GLSL 4.1, CMake, MIT, intended public as
`stoatworks-labs/5-wire`. Built from the regauss scaffolding 2026-08-24.

**RELEASED v0.1.0 2026-08-24. All six homes live**: repo
`github.com/stoatworks-labs/5-wire` (4 assets, macOS signed + notarised by the
autosign agent), project page `stoatworks-labs.com/software/5-wire/`, user
guide `/software/5-wire/guide/`, demo `5-wire-demo.stoatworks-labs.com`,
YouTube `CGt9p3fhMJc`, Instagram reel `DcZ-8NkDDW9`.

⚠️ **It has still never been loaded into Resolume, or any FFGL host** —
everything verified comes from `fwtest` driving the plugin class directly, so
the parameter declarations, the group layout and the About block are unproven
in a real inspector. Windows builds only in CI. **No OFX build.** No real cable
run has been photographed and compared against it.

**The one idea:** coax loses the SQUARE ROOT of frequency, and that has a
closed-form step response `erfc(alpha / 2*sqrt(t))` — strictly causal with a
t^-3/2 tail hundreds of pixels long. So edges smear only to the RIGHT, and the
soft picture and the streaking are the same curve. One parameter, `alpha`, from
cable type + length + pixel clock. Everything in the cable is in TIME; the
pixel clock is the only thing that turns a nanosecond into a pixel.

**The argument the plugin exists to make:** Pre-Emphasis and Cable EQ are the
same filter at opposite ends, and are NOT the same control — the noise and the
reflections join in between, so the receiver's equaliser lifts them with the
picture and the amplifier's does not, paying in headroom instead. That is why
the amplifier and the cable are two passes and not one convolution: an
amplifier has a RAIL, and the overshoot has to meet it before the cable smears
it. Six passes: Head, Line, Wide8, Wide64, Compose, Receive.

**Three things that were wrong first and are worth not repeating:**
- ☠️ **A Wiener/frequency-domain inverse for the equaliser degenerates into a
  MATCHED FILTER** where the cable has thrown the signal away, and a matched
  filter SMOOTHS — Cable EQ at maximum made a 100 m run *softer* than leaving
  it alone. Every other number looked healthy. It is now a regularised
  least-squares FIR (Cholesky on the autocorrelation), and `fwtest --eq`
  asserts directly that the cascade is never softer than the cable alone. An
  EXACT inverse is also wrong: its coefficients run to 1e11 past alpha 1.2.
- **Sync barely notices the cable.** `syncLoss = alpha * 0.9` put every long run
  into a rolling frame. An H sync pulse is tens of kHz against a hundred-MHz
  pixel clock; length buys JITTER (slower edge, less certain slicing), not loss
  of lock. Now `alpha * 0.18`, and roll comes from a low Sync Level instead.
- **Hum and ingress are additive and two-sided**, so an unscreened long run's
  pickup term reached 2.2 and rendered a white frame. Capped at 1.6.

**A test that discretisation defeats:** the kernel's frequency response cannot
be asserted against `exp(-alpha*sqrt(pi*f))` near Nyquist — the bins carry the
pixel's own aperture and the 640-px truncation lifts the low end. `--kernel`
asserts the exact properties (unit sum, causality, non-negativity, never a
gain) and bounds the measured -3 dB point at 0.7..1.4 of the analytic one,
which still catches a linear-f law. Do not "fix" that by band-limiting the
kernel: a band-limited kernel rings BEFORE the edge.

The strongest check is `--impulse`: one bright column through the whole chain,
compared against `Cable.cpp` tap by tap. It is what catches an upload that
silently did not happen — there is no GLSL twin of the model to drift, because
the shaders are convolutions that do not know what they convolve with.

**Two release traps this cut, both worth reusing:**
- **`OUTPUT_NAME` was inside the `if(APPLE)` block**, so macOS got
  `5-wire.bundle` and Windows quietly produced `FiveWire.dll` — clean compile,
  green log, and the only symptom was the release job's `cp: cannot stat` AFTER
  a tag would have landed. Caught by **dispatching `release.yml` manually
  before tagging**, which is what that trigger is for. Do that on any new repo
  where the CMake target name differs from the artefact name.
- The demo/video/site scripts in `stoatworks-backend` are split across
  branches: the checkout sits on `analytics-grafana` (38 ahead of local main),
  `origin/main` has the full `sync-about.py` TARGETS but a pre-reorg
  `resolume-demo/sync.sh`. Work from a **detached worktree of `origin/main`**
  and push there; do not touch the working checkout's branch.

⚠️ Still broken fleet-wide and NOT fixed here: `scripts/sync-attributions.py`
(`PROJECTS = ROOT.parent`) and `scripts/sync-funding.sh`
(`ROOT=$HOME/Projects`) still assume the pre-reorg flat layout and silently
process nothing — both report success. 5-wire's `ATTRIBUTIONS.md` is
hand-written and marked provisional as a result.

`fwtest --script` was added for the video: `frame Parameter Name value` lines,
same shape as rgtest's, held at the ends and linearly interpolated between.

30 controls, 10 presets, About block. Presets use the FIXED
[plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md) pattern (hostValues + seedHostValues) and
`fwtest --presets` passes all three host behaviours. See the repo's AGENTS.md
for the rest. Related: [regauss](https://github.com/stoatworks-labs/regauss/blob/main/docs/NOTES.md) (`regauss`) (the donor),
[new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md), [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md),
**release workflow** (working-practice note, kept in Claude memory).
