# 5-wire

A long run of VGA or RGBHV, and the amplifier at each end of it, as an FFGL
effect for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle`
(macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the cable model, the order of the passes, or
where anything is added to the signal.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/fwtest --out /tmp/frame.png`
- List parameters: `./build/fwtest --list`
- What the settings mean in metres and megahertz: `./build/fwtest --report`
- Set a control: `--set "Length=0.8" --set "Cable Type=3"` (by display name)
- A preset: `--set "Preset=2"`
- One bright column instead of the test card: `--impulse-picture`
- Put real footage through the real shaders:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/fwtest --pipe --width W --height H | ffmpeg …`

## Verify
- Everything: `tools/verify.sh`
- The cable's response against the law: `./build/fwtest --kernel`
- The equaliser against the cable: `./build/fwtest --eq`
- End to end, GPU against `Cable.cpp`: `./build/fwtest --impulse`
- Where the repeats land: `./build/fwtest --ghost`
- Presets against three host behaviours: `./build/fwtest --presets`
- No dead controls: `python3 tools/sweep.py`
- Universal + exports: `lipo -archs build/5-wire.bundle/Contents/MacOS/5-wire`
  and `nm -gU … | grep _plugMain`

## Notes
- **The cable is one filter, and it is not symmetric.** Coax loses the square
  root of frequency, whose step response is `erfc(alpha / 2*sqrt(t))` — strictly
  causal, with a tail hundreds of pixels long. A bright edge smears to the
  RIGHT and only to the right. That one curve is the soft picture *and* the
  streaking. See `AGENTS.md`.
- **Where a thing is added decides what happens to it.** Noise, reflections and
  crosstalk join the signal at the far end of the cable, so the equaliser at
  the receiver lifts them and the identical filter at the amplifier does not.
  That is the whole reason Pre-Emphasis and Cable EQ are two controls.
- **The equaliser is a length, not a frequency**, exactly like the knob on a
  real cable equaliser. Set it for the wrong length and you get the wrong
  answer in the right way.
- Everything in the cable is in TIME; `Pixel Clock` is the only thing that
  turns a nanosecond into a pixel. The same 30 m is invisible at 640x480 and a
  visible ghost at 1600x1200.
- **Roll and jitter are not controls.** They are what is left when the sync
  margin runs out. Turn Sync Level down and the frame runs.
- The macOS build must be universal (arm64 + x86_64). Verify with `lipo`,
  never the build log.
- Always override `SetTextParameter` to return FF_SUCCESS for the About block,
  or no host can instantiate the plugin.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. It names the pass, logs the GL
vendor/renderer/version, and records the host clock's unit once it is decided.

    ~/Library/Logs/5-wire/5-wire.YYYY-MM-DD.log
