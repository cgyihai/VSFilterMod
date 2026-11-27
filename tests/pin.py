# Regression tests for \pin(x1,y1,...,x4,y4) corner-pin perspective text (patch m021).
#
# The line layout box corners (clockwise from top-left) are mapped through a
# projective transform onto the four script-space corner points. These tests
# exploit equivariance properties of the homography so the unknown layout box
# never needs to be predicted:
#   * translating all four target corners translates the whole image exactly
#   * a rectangular target fully determines the output bounding geometry

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render_frames  # noqa: E402

BS = chr(92)

if len(sys.argv) != 3:
    print("usage: pin.py <vsfmrender> <workdir>")
    sys.exit(2)
RENDERER, WORK = sys.argv[1:]
FONTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "consistency", "fonts")

TEXT = "ABCDEFGHIJKLMNOP"
POS = BS + "pos(320,180)" + BS + "an5"


def bbox(frame):
    xs = [p[0] for p in frame["nonzero"]]
    ys = [p[1] for p in frame["nonzero"]]
    return min(xs), min(ys), max(xs), max(ys)


def centroid(frame):
    xs = [p[0] for p in frame["nonzero"]]
    ys = [p[1] for p in frame["nonzero"]]
    return sum(xs) / float(len(xs)), sum(ys) / float(len(ys))


def render(name, body):
    frames = render_frames(RENDERER, FONTDIR, WORK, name, body)
    assert frames, name + ": no frames"
    return frames[0]


def pin_tag(x1, y1, x2, y2, x3, y3, x4, y4):
    return BS + "pin(%d,%d,%d,%d,%d,%d,%d,%d)" % (x1, y1, x2, y2, x3, y3, x4, y4)


def run():
    flat = render("pin-flat", "{" + POS + "}" + TEXT)

    # Rectangle quads: width 200 x height 60, and the same shifted by (40, 15)
    rect_a = pin_tag(100, 100, 300, 100, 300, 160, 100, 160)
    rect_b = pin_tag(140, 115, 340, 115, 340, 175, 140, 175)
    pa = render("pin-rect-a", "{" + POS + rect_a + "}" + TEXT)
    pb = render("pin-rect-b", "{" + POS + rect_b + "}" + TEXT)

    # 1. translation equivariance: shifting all corners by (40,15) must shift
    # the ink centroid by the same amount within subpixel rounding
    cax, cay = centroid(pa)
    cbx, cby = centroid(pb)
    dx, dy = cbx - cax, cby - cay
    assert abs(dx - 40) < 1.5, "centroid x shift %.2f should be ~40" % dx
    assert abs(dy - 15) < 1.5, "centroid y shift %.2f should be ~15" % dy

    # 2. the two shifted copies are otherwise congruent: bbox sizes match
    wax0, way0, wax1, way1 = bbox(pa)
    wbx0, wby0, wbx1, wby1 = bbox(pb)
    assert abs((wax1 - wax0) - (wbx1 - wbx0)) <= 1, "shifted copy width mismatch"
    assert abs((way1 - way0) - (wby1 - wby0)) <= 1, "shifted copy height mismatch"

    # 3. scaling: a half-width rectangle quad halves the ink width
    half = pin_tag(100, 100, 200, 100, 200, 160, 100, 160)
    ph = render("pin-rect-half", "{" + POS + half + "}" + TEXT)
    whx0, why0, whx1, why1 = bbox(ph)
    wide = wax1 - wax0
    narrow = whx1 - whx0
    ratio = wide / float(max(narrow, 1))
    assert 1.7 < ratio < 2.3, "width ratio %.2f should be ~2 for half-width quad" % ratio

    # 4. perspective deformation: trapezoid with the right edge vertically
    # compressed to half; rightmost columns must be shorter than leftmost
    trap = pin_tag(100, 100, 300, 130, 300, 190, 100, 220)
    pt = render("pin-trap", "{" + POS + trap + "}" + TEXT)
    tx0, ty0, tx1, ty1 = bbox(pt)
    width = tx1 - tx0
    left_h, right_h = [], []
    heights = {}
    for x in range(tx0, tx1 + 1):
        ys = [p[1] for p in pt["nonzero"] if p[0] == x]
        if ys:
            heights[x] = max(ys) - min(ys) + 1
    left_xs = [x for x in heights if x < tx0 + width * 0.2]
    right_xs = [x for x in heights if x > tx1 - width * 0.2]
    if left_xs and right_xs:
        lh = sum(heights[x] for x in left_xs) / float(len(left_xs))
        rh = sum(heights[x] for x in right_xs) / float(len(right_xs))
        assert lh > rh + 5, "trapezoid left columns %.1f should exceed right %.1f" % (lh, rh)
    else:
        raise AssertionError("trapezoid ink columns missing")

    # 5. \pin0 disables: output bit-identical to flat text
    off = render("pin-off", "{" + POS + BS + "pin0}" + TEXT)
    assert off["hash"] == flat["hash"], "pin0 should be bit-identical to flat text"

    # 6. determinism: the same pinned line renders identically twice
    pa2 = render("pin-rect-a2", "{" + POS + rect_a + "}" + TEXT)
    assert pa["hash"] == pa2["hash"], "pin rendering must be deterministic"

    print("pin.py: all tests passed")


run()
