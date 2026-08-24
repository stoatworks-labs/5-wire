/**
 * 5-wire — browser demo.
 *
 * The shaders are not copied here. `shaders.js` is GENERATED from
 * `source/shaders/` by `demo/extract-shaders.mjs`, and `tools/verify.sh` fails
 * if it has drifted — so this page runs the plugin's own GLSL character for
 * character rather than a transcription of it that will one day fall behind.
 *
 * What IS ported by hand is the scalar half: `Cable.cpp` (the cable types, the
 * loss kernel and the equaliser design) and `Controls.cpp` (0..1 host values to
 * metres, hertz and ohms). Those are C++ the browser cannot link, and every one
 * of the ports below is marked against the function it mirrors.
 *
 * The thing worth understanding before reading any of it: **the cable is one
 * filter, and the filter is one-sided.** Coax loses the square root of
 * frequency, whose step response is erfc(alpha / 2*sqrt(t)) — strictly causal
 * with a tail hundreds of pixels long. So an edge smears to the RIGHT and only
 * to the right, and that same curve is both the soft picture and the streak
 * behind every caption. Everything else on the page is a consequence of where
 * in the chain a thing joins the signal: turn Cable EQ up and the noise and the
 * ghost come up with the picture, because they joined it before the equaliser;
 * turn Pre-Emphasis up instead and they do not, because it sits at the other
 * end.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import { VERTEX, HEAD, LINE, WIDE, COMPOSE, RECEIVE } from './shaders.js';

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const clamp01 = (v) => clamp(v, 0, 1);

//===========================================================================
// Port of source/Cable.cpp
//===========================================================================

/**
 * erfc, which the C++ gets from libm and JavaScript does not have at all.
 *
 * The series is the one with all-positive terms —
 *   erf(x) = (2/sqrt(pi)) * exp(-x^2) * SUM 2^n x^(2n+1) / (1*3*...*(2n+1))
 * — rather than the alternating Maclaurin series, which loses every significant
 * digit to cancellation by x = 3. Past about x = 5.9 this returns 0 where the
 * C++ returns a denormal; the kernel drops taps below 1e-4 anyway, so the two
 * agree everywhere either of them is used.
 */
function erfc(x) {
  if (x < 0) return 2 - erfc(-x);
  if (x > 6) return 0;

  let term = x;
  let sum = x;
  for (let n = 1; n < 200; ++n) {
    term *= (2 * x * x) / (2 * n + 1);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  const erf = (2 / Math.sqrt(Math.PI)) * Math.exp(-x * x) * sum;
  return 1 - erf;
}

/** Port of fivewire::stepAt(). */
function stepAt(alpha, t) {
  if (t <= 0) return 0;
  if (alpha <= 0) return 1;
  return erfc(alpha / (2 * Math.sqrt(t)));
}

const TAPS = 64;
const WIDE_TAPS = 8;
const WIDE_LEVELS = 2;
const WIDE_STRIDE = [8, 64];
const WIDE_START = [64, 128];
const DESIGN_LENGTH = 512;
const NOISE_FLOOR = 1e-4;
const MAX_GHOSTS = 4;

/**
 * The five kinds of run, from Cable.cpp.
 *
 * The loss figures are the published dB/100 m specifications for the cable each
 * entry stands for, converted to the sqrt(f) constant that produced them. The
 * crosstalk and shielding numbers are calibrated rather than measured — see the
 * plugin's AGENTS.md.
 */
const CABLES = [
  { name: 'RGBHV Coax', loss: 1.10, velocity: 0.66, skewNs: 1.0, skew: [0.6, 0.0, -0.6], crosstalk: 0.120, shielding: 1.00 },
  { name: 'VGA Lead', loss: 3.30, velocity: 0.68, skewNs: 3.0, skew: [0.5, 0.0, -0.5], crosstalk: 0.660, shielding: 0.45 },
  { name: 'Mini-Coax', loss: 2.20, velocity: 0.70, skewNs: 2.0, skew: [0.5, 0.0, -0.5], crosstalk: 0.260, shielding: 0.75 },
  { name: 'CAT5 Balun', loss: 2.00, velocity: 0.64, skewNs: 45.0, skew: [1.0, -0.2, -0.8], crosstalk: 0.850, shielding: 0.30 },
  { name: 'Ribbon Loom', loss: 4.50, velocity: 0.60, skewNs: 12.0, skew: [0.8, 0.1, -0.9], crosstalk: 1.900, shielding: 0.08 },
];

/** Port of fivewire::lossKernel(). */
function lossKernel(alpha) {
  const tap = new Float32Array(TAPS);
  const wide = [new Float32Array(WIDE_TAPS), new Float32Array(WIDE_TAPS)];

  if (alpha <= 0) {
    tap[0] = 1;
    return { tap, wide, headSum: 1 };
  }

  // Differencing the step response rather than sampling the impulse is not a
  // numerical nicety: the impulse response is t^-3/2 and goes infinite at the
  // origin, so a point sample of it near zero is meaningless while the integral
  // over the bin is exact.
  for (let n = 0; n < TAPS; ++n) tap[n] = stepAt(alpha, n + 0.5) - stepAt(alpha, n - 0.5);

  for (let level = 0; level < WIDE_LEVELS; ++level) {
    const stride = WIDE_STRIDE[level];
    const start = WIDE_START[level];
    for (let j = 0; j < WIDE_TAPS; ++j) {
      const a = start + stride * j;
      wide[level][j] = stepAt(alpha, a + stride) - stepAt(alpha, a);
    }
  }

  // Unit DC gain by construction. A cable does not dim the picture, and a
  // kernel that sums to 0.98 does exactly that while claiming to be passive.
  let total = 0;
  for (const t of tap) total += t;
  for (let level = 0; level < WIDE_LEVELS; ++level) for (const w of wide[level]) total += w;

  if (total > 1e-6) {
    const scale = 1 / total;
    for (let n = 0; n < TAPS; ++n) tap[n] *= scale;
    for (let level = 0; level < WIDE_LEVELS; ++level) {
      for (let j = 0; j < WIDE_TAPS; ++j) wide[level][j] *= scale;
    }
  }

  let headSum = 0;
  for (const t of tap) headSum += t;
  return { tap, wide, headSum };
}

/**
 * Port of fivewire::equaliserKernel(), memoised on alpha.
 *
 * Least squares and NOT an exact inverse. The exact inverse exists and is a
 * line of recursion, and past about alpha 1.2 its coefficients run to 1e11 —
 * 64 taps genuinely cannot undo a response six hundred pixels long. It is also
 * not a Wiener inverse, which was the first attempt: a Wiener solution
 * degenerates into a matched filter where the cable has thrown the signal away,
 * and a matched filter SMOOTHS, so the equaliser at maximum made a long run
 * softer than leaving it alone.
 *
 * The memo matters here in a way it does not in the plugin: this is a 64x64
 * Cholesky solve and the page runs it at frame rate, where the plugin runs it
 * once per ProcessOpenGL on a machine with rather more to spare.
 */
const eqCache = new Map();

function designEqualiser(alpha) {
  const key = Math.round(alpha * 4096);
  const hit = eqCache.get(key);
  if (hit) return hit;

  const taps = new Float64Array(TAPS);

  if (alpha > 0) {
    const h = new Float64Array(DESIGN_LENGTH);
    for (let n = 0; n < DESIGN_LENGTH; ++n) h[n] = stepAt(alpha, n + 0.5) - stepAt(alpha, n - 0.5);

    // R e = b, with R the autocorrelation of h (a symmetric Toeplitz matrix)
    // and b the single value h[0] in its first row, because the target is an
    // impulse at the origin.
    const acf = new Float64Array(TAPS);
    for (let lag = 0; lag < TAPS; ++lag) {
      let sum = 0;
      for (let n = 0; n + lag < DESIGN_LENGTH; ++n) sum += h[n] * h[n + lag];
      acf[lag] = sum;
    }

    const lower = new Float64Array(TAPS * TAPS);
    let ok = true;
    for (let i = 0; i < TAPS && ok; ++i) {
      for (let j = 0; j <= i; ++j) {
        let sum = acf[Math.abs(i - j)] + (i === j ? NOISE_FLOOR : 0);
        for (let k = 0; k < j; ++k) sum -= lower[i * TAPS + k] * lower[j * TAPS + k];
        if (i === j) {
          if (sum <= 0) { ok = false; break; }
          lower[i * TAPS + j] = Math.sqrt(sum);
        } else {
          lower[i * TAPS + j] = sum / lower[j * TAPS + j];
        }
      }
    }

    if (ok) {
      const forward = new Float64Array(TAPS);
      for (let i = 0; i < TAPS; ++i) {
        let sum = i === 0 ? h[0] : 0;
        for (let k = 0; k < i; ++k) sum -= lower[i * TAPS + k] * forward[k];
        forward[i] = sum / lower[i * TAPS + i];
      }
      for (let i = TAPS - 1; i >= 0; --i) {
        let sum = forward[i];
        for (let k = i + 1; k < TAPS; ++k) sum -= lower[k * TAPS + i] * taps[k];
        taps[i] = sum / lower[i * TAPS + i];
      }

      // An equaliser changes the balance of a picture, never its brightness.
      let sum = 0;
      for (const t of taps) sum += t;
      if (Math.abs(sum) > 1e-9) for (let i = 0; i < TAPS; ++i) taps[i] /= sum;
      else { taps.fill(0); taps[0] = 1; }
    } else {
      taps.fill(0);
      taps[0] = 1;
    }
  } else {
    taps[0] = 1;
  }

  // Bounded, because the page can be left open on one clip for an hour and the
  // length slider is continuous.
  if (eqCache.size > 256) eqCache.clear();
  eqCache.set(key, taps);
  return taps;
}

/** Port of fivewire::equaliserKernel()'s blend from flat. */
function equaliserKernel(alpha, amount) {
  const out = new Float32Array(TAPS);
  out[0] = 1;
  if (alpha <= 0 || amount === 0) return out;

  const designed = designEqualiser(alpha);
  for (let n = 0; n < TAPS; ++n) {
    const flat = n === 0 ? 1 : 0;
    out[n] = flat + amount * (designed[n] - flat);
  }
  return out;
}

/** Port of fivewire::alphaFor(). */
function alphaFor(spec, metres, pixelClockHz) {
  if (metres <= 0 || pixelClockHz <= 0) return 0;
  // dB/100 m/sqrt(MHz) -> the sqrt(seconds) constant of exp(-alpha*sqrt(pi*f)).
  const toNepers = 1 / (100 * 1000 * 8.685889638 * 1.772453851);
  return spec.loss * metres * toNepers * Math.sqrt(pixelClockHz);
}

/** Port of fivewire::transitPixels(). */
function transitPixels(spec, metres, pixelClockHz) {
  if (metres <= 0 || spec.velocity <= 0) return 0;
  return (metres / (spec.velocity * 299792458)) * pixelClockHz;
}

/** Port of fivewire::reflection(). Zero at 75 ohms, which is the point. */
const reflection = (ohms) => (ohms - 75) / (ohms + 75);

/** Port of fivewire::bandwidthCyclesPerPixel(). */
function bandwidthCyclesPerPixel(alpha) {
  if (alpha <= 0) return 1;
  const root = 0.34657359 / alpha;
  return (root * root) / Math.PI;
}

//===========================================================================
// Port of source/Controls.cpp
//===========================================================================

/** Metres of cable. Squared, so the useful resolution is at the bottom. */
const Metres = (p) => 150 * clamp01(p) * clamp01(p);

/** 25 MHz (640x480) to 340 MHz, logarithmic. */
const PixelClockHz = (p) => 25e6 * Math.pow(13.6, clamp01(p));

/** Geometric about 75, so the middle really is right rather than nearly right. */
const TerminationOhms = (p) => 75 * Math.pow(4, 2 * clamp01(p) - 1);

/** What Ghosting really sets: the mismatch looking back into the amplifier. */
const SourceReflection = (p) => clamp01(p) * 0.9;

const MainsHz = (option) => (option === 1 ? 60 : 50);

/** Stops below Nyquist, or the herringbone aliases into a fact about sampling. */
const IngressPitch = (p) => 0.02 + (0.45 - 0.02) * clamp01(p);

/** Port of the file-scope halfHeightPixels(). */
function halfHeightPixels(alpha) {
  const root = alpha / 0.95387;
  return root * root;
}

/** Port of significantTaps(). A short run is one tap and a rounding error. */
function significantTaps(taps) {
  let last = 0;
  for (let n = 0; n < TAPS; ++n) if (Math.abs(taps[n]) > 1e-4) last = n;
  return last + 1;
}

const TWO_PI = 6.283185307179586;

/** Port of fivewire::controls::drive(). */
function drive(s, time, framePeriod, outputWidth) {
  const spec = CABLES[clamp(Math.round(s.cableType), 0, CABLES.length - 1)];

  const metres = Metres(s.length);
  const clock = PixelClockHz(s.pixelClock);
  const alpha = alphaFor(spec, metres, clock);

  const d = {
    alpha,
    metres,
    transitPx: transitPixels(spec, metres, clock),
    bandwidth: bandwidthCyclesPerPixel(alpha),
  };

  const loss = lossKernel(alpha);
  const eqAlpha = alphaFor(spec, Metres(s.eqLength), clock);

  d.headKernel = equaliserKernel(eqAlpha, s.preEmphasis * 1.5);
  d.headTaps = significantTaps(d.headKernel);
  d.cableKernel = loss.tap;
  d.cableTaps = significantTaps(loss.tap);
  d.wide = loss.wide;
  d.useWide = loss.headSum < 0.995;

  d.eqKernel = equaliserKernel(eqAlpha, s.cableEq * 1.5);
  d.eqTaps = significantTaps(d.eqKernel);

  const skewSeconds = spec.skewNs * 1e-9 * (metres / 100);
  const skewPixels = skewSeconds * clock * s.skew * 2.5;
  d.skewPx = spec.skew.map((k) => k * skewPixels);
  d.splitConductors = Math.abs(d.skewPx[0] - d.skewPx[2]) > 0.05;

  const master = s.gain * 2;
  d.headGain = [master * s.red * 2, master * s.green * 2, master * s.blue * 2];
  d.headClip = 1 + s.headroom * 2;
  d.useHead =
    s.preEmphasis > 0.001 ||
    Math.abs(d.headGain[0] - 1) > 0.001 ||
    Math.abs(d.headGain[1] - 1) > 0.001 ||
    Math.abs(d.headGain[2] - 1) > 0.001;

  // A ghost needs a mismatch at BOTH ends: the far end to send energy back and
  // the near end to send it forward again. The product is what arrives.
  const gammaLoad = reflection(TerminationOhms(s.termination));
  const gammaSource = SourceReflection(s.ghosting);
  const product = gammaLoad * gammaSource;

  d.ghostAmp = new Float32Array(MAX_GHOSTS);
  d.ghostOffsetPx = new Float32Array(MAX_GHOSTS);
  d.ghostBlurPx = new Float32Array(MAX_GHOSTS);
  d.ghostCount = 0;
  for (let n = 1; n <= clamp(Math.round(s.bounces) + 1, 1, MAX_GHOSTS); ++n) {
    const offset = 2 * n * d.transitPx;
    if (offset > outputWidth) break;
    const amplitude = Math.pow(product, n);
    if (Math.abs(amplitude) < 0.002) break;
    const i = d.ghostCount++;
    d.ghostAmp[i] = amplitude;
    d.ghostOffsetPx[i] = offset;
    // Two extra transits per bounce, so the extra loss constant is 2n times
    // the cable's own.
    d.ghostBlurPx[i] = Math.min(64, halfHeightPixels(2 * n * alpha));
  }

  // A shield keeps the room out and a longer run is a longer aerial. Capped:
  // hum and ingress are additive and two-sided, and past this the artefact has
  // stopped being interference and become a light source.
  const shielding = clamp(spec.shielding * (0.5 + s.screening), 0, 1);
  const lengthAerial = clamp(Math.sqrt(metres / 25), 0, 2.5);
  const pickup = Math.min((1 - shielding) * lengthAerial, 1.6);

  d.crosstalk = clamp(spec.crosstalk * (metres / 100) * s.crosstalk * 1.5, 0, 2.5);

  // Thermal noise, which is the receiver's own and has nothing to do with the
  // shield. Added at the END of the cable, which is what makes the equaliser
  // lift it and pre-emphasis not.
  d.noise = s.noise * 0.12;

  const mains = MainsHz(Math.round(s.mains));
  d.hum = s.hum * 0.22 * pickup;
  d.humPerFrame = TWO_PI * mains * framePeriod;
  d.humPhase = (TWO_PI * mains * time) % TWO_PI;

  d.ingress = s.ingress * 0.18 * pickup;
  d.ingressPitch = IngressPitch(s.ingressPitch);
  d.ingressPhase = time * 37;

  // Sync barely notices the cable: an H sync pulse is tens of kilohertz against
  // a pixel clock of a hundred megahertz. What length takes is the EDGE, so it
  // buys jitter rather than loss of lock.
  d.syncDrive = s.syncLevel * 2;
  d.syncLoss = clamp(alpha * 0.18, 0, 0.6);
  d.sogAmount = s.syncOnGreen >= 0.5 ? 0.5 : 0;
  d.jitter = s.jitter * 8 * (0.5 + 0.5 * clamp(alpha, 0, 2));
  d.jitterSeed = (time * 60) % 4096;

  d.rollOffset = 0;
  const margin = d.syncDrive - d.syncLoss;
  if (margin < 0.35) {
    const rollRate = (0.35 - margin) * 2.5;
    d.rollOffset = (time * rollRate) % 1;
  }

  d.outGain = s.outputGain * 2;
  d.black = (s.black - 0.5) * 0.4;
  d.restore = s.restore;
  d.samplePhase = s.samplePhase;

  return d;
}

//===========================================================================
// The chain, mirroring FiveWire::ProcessOpenGL
//===========================================================================

function createRenderer(gl, quad) {
  const RGBA16F = gl.RGBA16F;

  const headShader = new Program(gl, VERTEX, HEAD, 'head');
  const lineShader = new Program(gl, VERTEX, LINE, 'line');
  const wideShader = new Program(gl, VERTEX, WIDE, 'wide');
  const composeShader = new Program(gl, VERTEX, COMPOSE, 'compose');
  const receiveShader = new Program(gl, VERTEX, RECEIVE, 'receive');

  const headBuffer = new PassBuffer(gl);
  const lineBuffer = new PassBuffer(gl);
  const wide8Buffer = new PassBuffer(gl);
  const wide64Buffer = new PassBuffer(gl);
  const composeBuffer = new PassBuffer(gl);

  return {
    dispose() {
      for (const p of [headShader, lineShader, wideShader, composeShader, receiveShader]) p.dispose();
      for (const b of [headBuffer, lineBuffer, wide8Buffer, wide64Buffer, composeBuffer]) b.dispose();
    },

    render({ params, input, width, height, time, framePeriod }) {
      const s = {
        cableType: params.get('cableType'),
        length: params.get('length'),
        pixelClock: params.get('pixelClock'),
        termination: params.get('termination'),
        ghosting: params.get('ghosting'),
        bounces: params.get('bounces'),
        skew: params.get('skew'),
        crosstalk: params.get('crosstalk'),
        screening: params.get('screening'),
        mains: params.get('mains'),
        noise: params.get('noise'),
        hum: params.get('hum'),
        ingress: params.get('ingress'),
        ingressPitch: params.get('ingressPitch'),
        syncLevel: params.get('syncLevel'),
        syncOnGreen: params.get('syncOnGreen'),
        jitter: params.get('jitter'),
        gain: params.get('gain'),
        red: params.get('red'),
        green: params.get('green'),
        blue: params.get('blue'),
        preEmphasis: params.get('preEmphasis'),
        eqLength: params.get('eqLength'),
        headroom: params.get('headroom'),
        cableEq: params.get('cableEq'),
        outputGain: params.get('outputGain'),
        black: params.get('black'),
        restore: params.get('restore'),
        samplePhase: params.get('samplePhase'),
      };

      const d = drive(s, time, framePeriod, width);

      const wide8W = Math.max(1, Math.floor((width + 7) / 8));
      const wide64W = Math.max(1, Math.floor((wide8W + 7) / 8));

      // The reduced buffers are wanted for three different reasons and any one
      // of them is enough: the tail of the cable's response, the clamp's
      // running average, and the green conductor's level when sync rides on it.
      const needWide = d.useWide || d.restore < 0.999 || d.sogAmount > 0;

      // 16-bit float throughout. Pre-emphasis overshoots well past white before
      // the cable brings it back, and the tail of the response is a term of a
      // few thousandths that has to survive being added.
      lineBuffer.ensure(width, height, RGBA16F);
      wide8Buffer.ensure(wide8W, height, RGBA16F);
      wide64Buffer.ensure(wide64W, height, RGBA16F);
      composeBuffer.ensure(width, height, RGBA16F);
      if (d.useHead) headBuffer.ensure(width, height, RGBA16F);

      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 1. The amplifier. Skipped at unity with no pre-emphasis, in which case
      //    the cable reads the source texture directly.
      //------------------------------------------------------------------
      let cableSource = input.texture;
      if (d.useHead) {
        headBuffer.bind();
        headShader.use();
        bindTexture(gl, 0, input.texture);
        headShader.setSampler('InputTexture', 0);
        headShader.set('MaxUV', 1, 1);
        headShader.set('OutputSize', width, height);
        headShader.setArray('Kernel', d.headKernel);
        headShader.setInt('TapCount', d.headTaps);
        headShader.set('HeadGain', d.headGain[0], d.headGain[1], d.headGain[2]);
        headShader.set('HeadClip', d.headClip);
        quad.draw();
        cableSource = headBuffer.texture;
      }

      //------------------------------------------------------------------
      // 2. The cable.
      //------------------------------------------------------------------
      lineBuffer.bind();
      lineShader.use();
      bindTexture(gl, 0, cableSource);
      lineShader.setSampler('SourceTexture', 0);
      lineShader.set('MaxUV', 1, 1);
      lineShader.set('OutputSize', width, height);
      lineShader.setArray('Kernel', d.cableKernel);
      lineShader.setInt('TapCount', d.cableTaps);
      lineShader.set('SkewPx', d.skewPx[0], d.skewPx[1], d.skewPx[2]);
      lineShader.setInt('Split', d.splitConductors ? 1 : 0);
      quad.draw();

      //------------------------------------------------------------------
      // 3. The two horizontal reductions. Horizontal ONLY: the cable's
      //    response is a fact about time, a scan line is the only axis
      //    carrying time, and reducing vertically would invent a coupling
      //    between lines that no cable has.
      //------------------------------------------------------------------
      if (needWide) {
        const reductions = [
          { target: wide8Buffer, source: lineBuffer.texture, width: wide8W },
          { target: wide64Buffer, source: wide8Buffer.texture, width: wide64W },
        ];
        for (const pass of reductions) {
          pass.target.bind();
          wideShader.use();
          bindTexture(gl, 0, pass.source);
          wideShader.setSampler('SourceTexture', 0);
          wideShader.set('OutTexel', 1 / pass.width);
          quad.draw();
        }
      }

      //------------------------------------------------------------------
      // 4. The far end of the cable.
      //------------------------------------------------------------------
      composeBuffer.bind();
      composeShader.use();
      bindTexture(gl, 0, lineBuffer.texture);
      bindTexture(gl, 1, wide8Buffer.texture);
      bindTexture(gl, 2, wide64Buffer.texture);
      composeShader.setSampler('LineTexture', 0);
      composeShader.setSampler('Wide8Texture', 1);
      composeShader.setSampler('Wide64Texture', 2);
      composeShader.set('OutputSize', width, height);

      composeShader.setArray('Wide8W', d.wide[0]);
      composeShader.setArray('Wide64W', d.wide[1]);
      composeShader.setInt('UseWide', d.useWide ? 1 : 0);

      composeShader.setArray('GhostAmp', d.ghostAmp);
      composeShader.setArray('GhostOffset', d.ghostOffsetPx);
      composeShader.setArray('GhostBlur', d.ghostBlurPx);
      composeShader.setInt('GhostCount', d.ghostCount);

      composeShader.set('Crosstalk', d.crosstalk);
      composeShader.set('Noise', d.noise);
      composeShader.set('Hum', d.hum);
      composeShader.set('HumPerFrame', d.humPerFrame);
      composeShader.set('HumPhase', d.humPhase);
      composeShader.set('Ingress', d.ingress);
      composeShader.set('IngressPitch', d.ingressPitch);
      composeShader.set('IngressPhase', d.ingressPhase);

      composeShader.set('SyncDrive', d.syncDrive);
      composeShader.set('SyncLoss', d.syncLoss);
      composeShader.set('SogAmount', d.sogAmount);
      composeShader.set('Jitter', d.jitter);
      composeShader.set('JitterSeed', d.jitterSeed);
      composeShader.set('RollOffset', d.rollOffset);
      quad.draw();

      bindTexture(gl, 2, null);
      bindTexture(gl, 1, null);

      //------------------------------------------------------------------
      // 5. The receiver: equaliser, clamp, sampler. AFTER the compose pass,
      //    and that is the point of the whole plugin.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      receiveShader.use();
      bindTexture(gl, 0, composeBuffer.texture);
      bindTexture(gl, 1, wide64Buffer.texture);
      receiveShader.setSampler('ComposeTexture', 0);
      receiveShader.setSampler('Wide64Texture', 1);
      receiveShader.set('OutputSize', width, height);
      receiveShader.setArray('Kernel', d.eqKernel);
      receiveShader.setInt('TapCount', d.eqTaps);
      receiveShader.set('SamplePhase', d.samplePhase);
      receiveShader.set('Restore', d.restore);
      receiveShader.set('Black', d.black);
      receiveShader.set('OutGain', d.outGain);
      quad.draw();

      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);
    },
  };
}

//===========================================================================

const pct = (v) => `${Math.round(v * 100)}%`;
const unity = (v) => `${(v * 2).toFixed(2)}×`;

mountDemo({
  name: '5-wire',
  pluginId: '5W01',
  tagline: 'A long run of VGA or RGBHV, and the amplifier at each end of it.',
  repo: 'https://github.com/stoatworks-labs/5-wire',
  page: 'https://stoatworks-labs.com/software/5-wire/',
  needFloat: true,
  showBackdrop: true,

  params: [
    // ---- Cable -----------------------------------------------------------
    {
      id: 'cableType', name: 'Cable Type', type: 'option', default: 1, group: 'Cable',
      elements: CABLES.map((c) => c.name),
      hint: 'Loss, velocity, conductor skew and screening, all from the one choice. Coax is near enough transparent at any length a room has; the moulded lead in the flight case is three times worse and lets far more of the room in; a CAT5 balun fringes colour that coax at the same length does not, because its pairs are different lengths by design.',
    },
    {
      id: 'length', name: 'Length', type: 'standard', default: 0.35, group: 'Cable',
      display: (v) => `${Metres(v).toFixed(1)} m`,
      hint: 'Squared, so the bottom of the slider has the resolution. The difference between 2 m and 10 m is nothing; the difference between 90 m and 100 m is whether the show works.',
    },
    {
      id: 'pixelClock', name: 'Pixel Clock', type: 'standard', default: 0.56, group: 'Cable',
      display: (v) => `${(PixelClockHz(v) / 1e6).toFixed(0)} MHz`,
      hint: 'Not decoration. Everything in the cable happens in TIME, and the pixel clock is the only thing that turns a nanosecond into a pixel — so the same 30 m lead is invisible at 640×480 and a plainly visible ghost at 1600×1200.',
    },
    {
      id: 'termination', name: 'Termination', type: 'standard', default: 0.70, group: 'Cable',
      display: (v) => `${TerminationOhms(v).toFixed(0)} Ω`,
      hint: 'Half the slider is each way of being wrong and the middle really is right. Below 75 the repeat is DARK (someone left a terminator on a through connection); above it, bright (a high-impedance input with nothing terminating the run).',
    },
    {
      id: 'ghosting', name: 'Ghosting', type: 'standard', default: 0.35, group: 'Cable',
      display: (v) => `Γ ${SourceReflection(v).toFixed(2)}`,
      hint: 'What this really sets is the mismatch looking back into the amplifier. A ghost needs TWO mismatches — the far end to send it back and the amplifier to send it forward again — so zero here is a correctly back-matched output stage and there is no ghost at any termination.',
    },
    {
      id: 'bounces', name: 'Bounces', type: 'option', default: 0, group: 'Cable',
      elements: ['1', '2', '3', '4'],
      hint: 'Past the fourth the round trip has been down the cable nine times and there is nothing left of it.',
    },
    {
      id: 'skew', name: 'Skew', type: 'standard', default: 0.35, group: 'Cable',
      display: pct,
      hint: 'How far apart the three conductors are in length. Near zero on coax cut from one drum — genuinely dead there, and that is what coax is for.',
    },
    {
      id: 'crosstalk', name: 'Crosstalk', type: 'standard', default: 0.40, group: 'Cable',
      display: pct,
      hint: 'A conductor couples its neighbour’s RATE OF CHANGE, not its level, so this appears as coloured outlines on edges and never as a tint over flat colour. Which is the difference between crosstalk and a bad white balance.',
    },
    {
      id: 'screening', name: 'Screening', type: 'standard', default: 0.50, group: 'Cable',
      display: pct,
      hint: 'Scales hum and radio together, because they arrive by the same route — there is no cable that is good at keeping mains out and bad at keeping radio out. Half is the cable type’s own specification.',
    },

    // ---- Interference ----------------------------------------------------
    { id: 'noise', name: 'Noise', type: 'standard', default: 0.22, group: 'Interference', display: pct, hint: 'The receiver’s own thermal noise, added at the END of the cable. Which is why Cable EQ lifts it and Pre-Emphasis does not.' },
    { id: 'hum', name: 'Hum', type: 'standard', default: 0.35, group: 'Interference', display: pct, hint: 'On the sync conductor as well as the picture, which is why the bar bends the lines it passes through instead of only dimming them.' },
    {
      id: 'mains', name: 'Mains', type: 'option', default: 0, group: 'Interference',
      elements: ['50 Hz', '60 Hz'],
      hint: 'Mains at the frame rate puts every line at the same phase and the bar stands still. A hertz either side and it crawls.',
    },
    { id: 'ingress', name: 'Ingress', type: 'standard', default: 0.20, group: 'Interference', display: pct, hint: 'A transmitter down the road. Each line catches the carrier at a different phase, which is what turns it into a herringbone rather than vertical stripes.' },
    { id: 'ingressPitch', name: 'Ingress Pitch', type: 'standard', default: 0.35, group: 'Interference', display: (v) => `${IngressPitch(v).toFixed(3)} c/px` },

    // ---- Sync ------------------------------------------------------------
    {
      id: 'syncLevel', name: 'Sync Level', type: 'standard', default: 0.75, group: 'Sync',
      display: unity,
      hint: 'How hard the amplifier drives H and V. There is no Roll control — turn this down far enough and the receiver loses vertical lock, because that is where a rolling frame comes from.',
    },
    { id: 'syncOnGreen', name: 'Sync On Green', type: 'boolean', default: 0, group: 'Sync', hint: 'Sync rides on the green conductor, so a bright green line loads the sync tip and the picture wobbles on exactly the shots that are green.' },
    { id: 'jitter', name: 'Jitter', type: 'standard', default: 0.30, group: 'Sync', display: pct, hint: 'The receiver’s own timing, plus what a slow sync edge costs it. A long run buys jitter; it does not, on its own, lose lock.' },

    // ---- Amplifier -------------------------------------------------------
    { id: 'gain', name: 'Gain', type: 'standard', default: 0.50, group: 'Amplifier', display: unity },
    { id: 'red', name: 'Red', type: 'standard', default: 0.50, group: 'Amplifier', display: unity },
    { id: 'green', name: 'Green', type: 'standard', default: 0.50, group: 'Amplifier', display: unity },
    { id: 'blue', name: 'Blue', type: 'standard', default: 0.50, group: 'Amplifier', display: unity },
    {
      id: 'preEmphasis', name: 'Pre-Emphasis', type: 'standard', default: 0.0, group: 'Amplifier',
      display: pct,
      hint: 'The same filter as Cable EQ, applied at the other end. It lifts nothing but the picture, because the noise and the reflections have not joined it yet — and pays in headroom instead, because it overshoots every edge into the rail.',
    },
    {
      id: 'eqLength', name: 'EQ Length', type: 'standard', default: 0.35, group: 'Amplifier',
      display: (v) => `${Metres(v).toFixed(1)} m`,
      hint: 'A real cable equaliser is calibrated in metres, and so is this one. Set it short and the picture stays soft; set it long and it rings, with a bright outline on the trailing side of every edge.',
    },
    { id: 'headroom', name: 'Headroom', type: 'standard', default: 0.50, group: 'Amplifier', display: (v) => `${(1 + v * 2).toFixed(2)}×`, hint: 'Where the output stage runs out of rail. Only bites with pre-emphasis up, which is the whole trade.' },

    // ---- Receiver --------------------------------------------------------
    {
      id: 'cableEq', name: 'Cable EQ', type: 'standard', default: 0.30, group: 'Receiver',
      display: pct,
      hint: 'The inverse of the cable, applied at the display — AFTER the noise, the ghost and the crosstalk joined the signal, so it lifts all of them with the picture. Compare it against Pre-Emphasis on a long run: same sharpness, very different noise floor.',
    },
    { id: 'outputGain', name: 'Output Gain', type: 'standard', default: 0.50, group: 'Receiver', display: unity },
    { id: 'black', name: 'Black Level', type: 'standard', default: 0.50, group: 'Receiver', display: (v) => `${((v - 0.5) * 0.4 * 100).toFixed(0)} IRE` },
    {
      id: 'restore', name: 'DC Restore', type: 'standard', default: 0.80, group: 'Receiver',
      display: pct,
      hint: 'A video signal is AC coupled; without a working clamp its average is forced to zero, so a bright area pushes everything after it DOWN. That is streaking, and it is why an overexposed caption leaves a dark trail across the rest of the line.',
    },
    {
      id: 'samplePhase', name: 'Sample Phase', type: 'standard', default: 0.0, group: 'Receiver',
      display: (v) => `${v.toFixed(2)} px`,
      hint: 'The “auto adjust” button on the front of every VGA monitor. At the right phase the receiver samples where the signal has settled; half a pixel out it samples the transition, and fine detail loses contrast and shimmers.',
    },
  ],

  sources: ['bars', 'detail', 'scene', 'spot', 'grid', 'ramp', 'alpha'],

  presets: {
    'House Run': { cableType: 0, length: 0.45, termination: 0.50, ghosting: 0.20, bounces: 0, skew: 0.20, crosstalk: 0.15, screening: 0.60, noise: 0.08, hum: 0.05, ingress: 0.03, syncLevel: 0.85, jitter: 0.05, preEmphasis: 0, eqLength: 0.45, headroom: 0.60, cableEq: 0, restore: 0.95, samplePhase: 0 },
    'Long Haul': { cableType: 2, length: 0.82, termination: 0.55, ghosting: 0.30, bounces: 0, skew: 0.30, crosstalk: 0.35, screening: 0.50, noise: 0.15, hum: 0.20, ingress: 0.10, syncLevel: 0.70, jitter: 0.25, preEmphasis: 0, eqLength: 0.82, headroom: 0.50, cableEq: 0, restore: 0.55, samplePhase: 0 },
    'Equalised': { cableType: 2, length: 0.82, termination: 0.55, ghosting: 0.30, bounces: 0, skew: 0.30, crosstalk: 0.35, screening: 0.50, noise: 0.15, hum: 0.20, ingress: 0.10, syncLevel: 0.70, jitter: 0.25, preEmphasis: 0, eqLength: 0.82, headroom: 0.50, cableEq: 0.85, restore: 0.55, samplePhase: 0 },
    'Pre-Emphasised': { cableType: 2, length: 0.82, termination: 0.55, ghosting: 0.30, bounces: 0, skew: 0.30, crosstalk: 0.35, screening: 0.50, noise: 0.15, hum: 0.20, ingress: 0.10, syncLevel: 0.70, jitter: 0.25, preEmphasis: 0.80, eqLength: 0.82, headroom: 0.20, cableEq: 0, restore: 0.55, samplePhase: 0 },
    'Unterminated': { cableType: 0, length: 0.60, termination: 0.86, ghosting: 0.55, bounces: 2, skew: 0.20, crosstalk: 0.20, screening: 0.60, noise: 0.10, hum: 0.08, ingress: 0.05, syncLevel: 0.80, jitter: 0.10, preEmphasis: 0, eqLength: 0.60, headroom: 0.50, cableEq: 0, restore: 0.90, samplePhase: 0 },
    'Skip Lead': { cableType: 1, length: 0.42, termination: 0.72, ghosting: 0.45, bounces: 1, skew: 0.40, crosstalk: 0.55, screening: 0.30, noise: 0.35, hum: 0.65, ingress: 0.45, ingressPitch: 0.28, syncLevel: 0.65, jitter: 0.40, green: 0.48, blue: 0.46, preEmphasis: 0, eqLength: 0.42, headroom: 0.50, cableEq: 0.25, outputGain: 0.52, restore: 0.70, samplePhase: 0.35 },
    'CAT5 Extender': { cableType: 3, length: 0.70, termination: 0.60, ghosting: 0.35, bounces: 0, skew: 0.85, crosstalk: 0.60, screening: 0.35, noise: 0.25, hum: 0.30, ingress: 0.35, ingressPitch: 0.40, syncLevel: 0.60, syncOnGreen: 1, jitter: 0.35, preEmphasis: 0, eqLength: 0.70, headroom: 0.50, cableEq: 0.45, restore: 0.75, samplePhase: 0 },
    'Losing Sync': { cableType: 1, length: 0.62, termination: 0.65, ghosting: 0.40, bounces: 1, skew: 0.45, crosstalk: 0.50, screening: 0.30, noise: 0.30, hum: 0.45, ingress: 0.25, ingressPitch: 0.32, syncLevel: 0.20, syncOnGreen: 1, jitter: 0.85, preEmphasis: 0, eqLength: 0.62, headroom: 0.50, cableEq: 0.30, restore: 0.65, samplePhase: 0.20 },
    'Tired Amp': { cableType: 1, length: 0.50, termination: 0.68, ghosting: 0.50, bounces: 1, skew: 0.40, crosstalk: 0.45, screening: 0.35, noise: 0.30, hum: 0.40, ingress: 0.20, syncLevel: 0.55, jitter: 0.45, gain: 0.46, red: 0.545, green: 0.50, blue: 0.455, preEmphasis: 0.35, eqLength: 0.75, headroom: 0.15, cableEq: 0.55, outputGain: 0.58, black: 0.44, restore: 0.15, samplePhase: 0.50 },
    'Dead Run': { cableType: 4, length: 0.95, termination: 0.86, ghosting: 0.60, bounces: 2, skew: 0.90, crosstalk: 0.90, screening: 0.15, noise: 0.35, hum: 0.60, ingress: 0.50, ingressPitch: 0.42, syncLevel: 0.15, syncOnGreen: 1, jitter: 0.95, gain: 0.50, red: 0.52, green: 0.48, blue: 0.55, preEmphasis: 0, eqLength: 0.55, headroom: 0.35, cableEq: 0.50, outputGain: 0.50, black: 0.50, restore: 0.05, samplePhase: 0.45 },
  },

  differences: [
    'The plugin has a Preset dropdown as a real parameter, declared after the controls so a saved composition’s parameter ids do not shift. This page has the same ten looks as buttons instead, because a browser has no composition to save.',
    'C++ gets erfc — the whole cable model rests on it — from libm. JavaScript has no such function, so the page carries its own series implementation. It agrees with the C++ to about twelve digits below x = 3 and returns zero past about x = 5.9, where the plugin returns a denormal; the kernel drops taps below 1e-4 either way, so the two never disagree anywhere the answer is used.',
    'The equaliser is a 64×64 Cholesky solve, and the plugin runs it once per rendered frame in C++. This page memoises the design on the loss constant, so dragging Length or EQ Length re-solves and holding them still does not. The result is identical; the timing is not.',
    'The plugin asks the host for its clock, so re-rendering a composition gives the same noise rather than whatever the wall clock said. Here the clock is the page’s own, accumulated from frame deltas — which is why Restart puts the hum bar back where it started.',
    'Whether the hum bar rolls or stands still depends on the frame period, and the plugin measures that from the host. Here it is measured from the browser’s, so the same Mains setting will crawl at a different rate on a 60 Hz display than on a 120 Hz one. That is the real behaviour, not an artefact: 50 Hz on a set scanning at 50 Hz genuinely does not move.',
    'The chain runs in 16-bit float here as it does in the plugin, which needs EXT_color_buffer_float. If your browser lacks it the page says so rather than quietly dropping to 8 bits and banding the response’s tail into steps.',
  ],

  createRenderer,
});
