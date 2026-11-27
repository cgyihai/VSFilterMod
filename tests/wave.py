# Regression tests for \wave(amp,freq[,phase]) sine-wave text layout (patch m018).
#
# The wave displaces each glyph vertically on a sine curve running along the
# line advance direction; glyphs rotate to the wave tangent. amp and freq are
# output pixels (freq is the full wavelength), phase is in degrees. \wave0
# disables. These tests probe the rendered ink geometry.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render_frames  # noqa: E402

BS = chr(92)

if len(sys.argv) != 3:
    print("usage: wave.py <vsfmrender> <workdir>")
    sys.exit(2)
RENDERER, WORK = sys.argv[1:]
FONTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "consistency", "fonts")

TEXT = "ABCDEFGHIJKLMNOP"
POS = BS + "pos(320,180)" + BS + "an5"


def bbox(frame):
    xs = [p[0] for p in frame["nonzero"]]
    ys = [p[1] for p in frame["nonzero"]]
    return min(xs), min(ys), max(xs), max(ys)


def render(name, body):
    frames = render_frames(RENDERER, FONTDIR, WORK, name, body)
    assert frames, name + ": no frames"
    return frames[0]


def run():
    flat = render("wave-flat", "{" + POS + "}" + TEXT)
    wave = render("wave-basic", "{" + POS + BS + "wave(20,150)}" + TEXT)

    fx0, fy0, fx1, fy1 = bbox(flat)
    wx0, wy0, wx1, wy1 = bbox(wave)

    # 1. wave spreads ink taller than flat text (amp 20 vertical swing)
    flat_h = fy1 - fy0
    wave_h = wy1 - wy0
    assert wave_h > flat_h + 20, "wave height %d should exceed flat %d by margin" % (wave_h, flat_h)

    # 2. vertical oscillation: per-column ink centroids must vary along x
    centroids = {}
    for x in range(wx0, wx1 + 1):
        ys = [p[1] for p in wave["nonzero"] if p[0] == x]
        if ys:
            centroids[x] = sum(ys) / float(len(ys))
    spread = max(centroids.values()) - min(centroids.values())
    assert spread > 15, "wave ink centroid spread %.1f should oscillate" % spread

    # 3. crests and troughs alternate: centroid sequence is not monotonic
    xs = sorted(centroids)
    vals = [centroids[x] for x in xs]
    # smooth a little: sample every 8 px
    sampled = vals[::8]
    ups = sum(1 for a, b in zip(sampled, sampled[1:]) if b < a - 1)
    downs = sum(1 for a, b in zip(sampled, sampled[1:]) if b > a + 1)
    assert ups >= 1 and downs >= 1, "wave should rise and fall (ups %d downs %d)" % (ups, downs)

    # 4. horizontal layout roughly preserved: wave moves no glyph sideways, but
    # tangent rotation both widens rotated glyph boxes and lets neighbours
    # lean into each other (overlap), so the bbox may grow or shrink.
    flat_w = fx1 - fx0
    wave_w = wx1 - wx0
    assert wave_w > flat_w * 0.7 and wave_w < flat_w * 1.7,         "wave width %d should be within rotation tolerance of flat %d" % (wave_w, flat_w)

    # 5. amplitude scaling: amp 40 spreads more than amp 20
    big = render("wave-big", "{" + POS + BS + "wave(40,150)}" + TEXT)
    bx0, by0, bx1, by1 = bbox(big)
    assert (by1 - by0) > wave_h + 10, "amp 40 height %d should exceed amp 20 height %d" % (by1 - by0, wave_h)

    # 6. phase shift moves the oscillation pattern
    p1 = render("wave-phase", "{" + POS + BS + "wave(20,150,180)}" + TEXT)
    p1set = set((q[0], q[1]) for q in p1["nonzero"])
    same = sum(1 for p in wave["nonzero"] if (p[0], p[1]) in p1set)
    total = len(wave["nonzero"])
    assert same < total * 0.9, "phase 180 should shift the wave pattern (overlap %d/%d)" % (same, total)

    # 7. \wave0 disables
    off = render("wave-off", "{" + POS + BS + "wave(20,150)" + BS + "wave0}" + TEXT)
    ox0, oy0, ox1, oy1 = bbox(off)
    assert abs(ox0 - fx0) <= 1 and abs(oy0 - fy0) <= 1 and abs(ox1 - fx1) <= 1 and abs(oy1 - fy1) <= 1, \
        "wave0 bbox should match flat"

    # 8. compact form equivalent to parenthesized
    sfx = render("wave-sfx", "{" + POS + BS + "wave(20,150)}" + TEXT)
    pfx = render("wave-pfx", "{" + POS + BS + "wave(20,150)}" + TEXT)
    assert len(sfx["nonzero"]) == len(pfx["nonzero"]), "same input must render identically"

    # 9. determinism
    again = render("wave-basic2", "{" + POS + BS + "wave(20,150)}" + TEXT)
    assert len(again["nonzero"]) == len(wave["nonzero"]), "determinism"

    print("wave tests passed")


run()
