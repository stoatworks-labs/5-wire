# Attributions

5-wire is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is normally generated — the master lists live in the `stoatworks-backend`
repo and are pushed out by `scripts/sync-attributions.py`. Edit it there, not
here.

> **This copy is hand-written and provisional.** The generator skips any repo
> without a `stoatworks-labs/` origin remote, and this one has no remote yet.
> Run the sync once the repo is pushed and it will replace this file wholesale.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked by the offline harness only, and from the copy that ships with macOS.
`tools/fwtest` writes PNGs, and a PNG is a few chunk headers around a deflate
stream; using the system zlib is why that is fifty lines here rather than a
vendored dependency.

## Prior art and sources

Nothing in this repo is derived from anyone else's code, but the model is not
invented. The physics is standard transmission-line theory:

- **The Heaviside cable equation.** The step response of a conductor whose
  attenuation goes as the square root of frequency is `erfc(α / 2√t)`. This is
  the nineteenth-century result that made long-distance telegraphy work, and it
  is the single equation this plugin is built on.
- **Cable attenuation figures** are the published dB/100 m specifications for
  the cable each type stands for, converted to the square-root constant that
  produced them.
- **Reflection coefficients** are `Γ = (Z − Z₀)/(Z + Z₀)` against the 75 Ω the
  line was built for, with a ghost being the product of the coefficients at both
  ends.
