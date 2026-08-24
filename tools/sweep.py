"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**The noise is deterministic, and it has to be.** Every random thing in this
plugin is hashed from the pixel and from the frame's own clock, so two renders
of the same frame count produce the same grain. If it were seeded from a wall
clock instead, every comparison here would differ regardless and this test would
pass for a plugin with no working controls at all.

**Half the controls are properties of something else being on.** Termination
does nothing with Ghosting at zero -- correctly, because a ghost needs a
mismatch at both ends. Ingress Pitch does nothing with Ingress at zero. The
baseline therefore has every stage active rather than being the defaults.

**Skew is genuinely dead on coax**, which is the point of coax: five cables cut
from one drum are the same length. It gets a CONTEXT entry that puts a CAT5
balun on the floor, where the pairs are different lengths by design.

**Render wide enough for the repeats to land on screen.** A reflection further
out than the picture is wide is dropped before it is drawn, so at 640 px several
of the ghost controls read dead and are right to.

**Never sweep the About block.** Those parameters are buttons that open a web
browser, and sweeping them opens one tab per press.
"""
import subprocess, zlib, struct, sys, tempfile

SC = tempfile.mkdtemp(prefix="fwsweep")
BIN = "./build/fwtest"

WIDTH, HEIGHT = 1280, 720

# A baseline with every stage active, so nothing reads dead merely because the
# thing it modifies is switched off. A cheap lead, half terminated, in a room
# with mains in it.
BASE = {
    "Cable Type": 1, "Length": 0.5, "Pixel Clock": 0.56,
    "Termination": 0.75, "Ghosting": 0.7, "Bounces": 1,
    "Skew": 0.5, "Crosstalk": 0.6, "Screening": 0.25,
    "Noise": 0.3, "Hum": 0.45, "Ingress": 0.35, "Ingress Pitch": 0.35,
    "Sync Level": 0.6, "Jitter": 0.4,
    "Gain": 0.5, "Pre-Emphasis": 0.3, "EQ Length": 0.5, "Headroom": 0.5,
    "Cable EQ": 0.4, "Output Gain": 0.5, "DC Restore": 0.5,
}

# Frames at 60 fps. One second, which is long enough for the mains phase to
# have moved somewhere different at 50 Hz than at 60.
FRAMES, FPS = 60, 60

# Parameters that need the world arranged differently before they can be seen
# at all. Keys: "set" (extra overrides), "frames", "fps".
CONTEXT = {
    # Coax has no skew worth having and that is what coax is FOR. Put a balun
    # on the floor, where the pairs are twisted at different rates and are
    # therefore genuinely different lengths.
    "Skew": {"set": {"Cable Type": 3, "Length": 0.7}},

    # A second and a third repeat need enough mismatch at both ends to still
    # be visible after three and five trips down the cable.
    "Bounces": {"set": {"Termination": 1.0, "Ghosting": 1.0, "Length": 0.45}},

    # Headroom is where the amplifier runs out of rail, so it needs something
    # driving into the rail: full pre-emphasis, which overshoots every edge.
    "Headroom": {"set": {"Pre-Emphasis": 1.0, "Gain": 0.62}},

    # Sync on green only matters when the margin is thin enough for a bright
    # green line to matter. With a healthy amplifier it is correctly nothing.
    "Sync On Green": {"set": {"Sync Level": 0.32}},

    # The mains frequency decides whether the bar sits still or crawls, which
    # is a difference that needs more than one frame to exist.
    "Mains": {"set": {"Hum": 0.8, "Screening": 0.0}, "frames": 45},
}

# Options are discrete; sweep them across their real element range.
DISCRETE = {
    "Cable Type": (0, 4), "Bounces": (0, 3), "Mains": (0, 1),
    "Sync On Green": (0, 1), "Preset": (0, 10),
}


def render(path, overrides, frames, fps):
    args = [BIN, "--out", path, "--width", str(WIDTH), "--height", str(HEIGHT),
            "--frames", str(frames), "--fps", str(fps)]
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    return open(path, "rb").read()


def pixels(png):
    i = 8
    idat = b""
    w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


listing = subprocess.run([BIN, "--list"], capture_output=True, text=True).stdout
params = [" ".join(l.split()[1:-1]) for l in listing.strip().splitlines()]

# Everything from the About text line onwards is the Stoatworks About block:
# one display-only string and a row of buttons that open a web browser. Not
# controls, and pressing them is not a test.
if "About" in params:
    params = params[:params.index("About")]

print(f"{'parameter':<20} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    ctx = CONTEXT.get(p, {})
    extra = ctx.get("set", {})
    frames = ctx.get("frames", FRAMES)
    fps = ctx.get("fps", FPS)

    a = render(f"{SC}/a.png", {**extra, p: lo}, frames, fps)
    b = render(f"{SC}/b.png", {**extra, p: hi}, frames, fps)
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok:
        dead.append(p)
    print(f"{p:<20} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
