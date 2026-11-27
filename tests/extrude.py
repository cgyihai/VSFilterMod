# Regression tests for \extrude(depth[,angle]) 3D extrusion (patch m019).
#
# The extrusion paints shadow-colour copies of the word at 1px steps along
# the extrusion direction, forming the solid side of a 3D effect. Copies
# render even with zero shadow depth. These tests probe rendered ink geometry.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render_frames  # noqa: E402

BS = chr(92)

if len(sys.argv) != 3:
    print("usage: extrude.py <vsfmrender> <workdir>")
    sys.exit(2)
RENDERER, WORK = sys.argv[1:]
FONTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "consistency", "fonts")

TEXT = "EXTRUDE"
POS = BS + "pos(320,180)" + BS + "an5" + BS + "bord0" + BS + "shad0"


def bbox(frame):
    xs = [p[0] for p in frame["nonzero"]]
    ys = [p[1] for p in frame["nonzero"]]
    return min(xs), min(ys), max(xs), max(ys)


def render(name, body):
    frames = render_frames(RENDERER, FONTDIR, WORK, name, body)
    assert frames, name + ": no frames"
    return frames[0]


def run():
    flat = render("ex-flat", "{" + POS + "}" + TEXT)
    ext = render("ex-basic", "{" + POS + BS + "extrude(20,45)}" + TEXT)

    fx0, fy0, fx1, fy1 = bbox(flat)
    ex0, ey0, ex1, ey1 = bbox(ext)

    # 1. extrusion extends ink down-right beyond the flat bbox (angle 45, y down)
    assert ex1 > fx1 + 8, "extrude right %d should exceed flat right %d" % (ex1, fx1)
    assert ey1 > fy1 + 8, "extrude bottom %d should exceed flat bottom %d" % (ey1, fy1)

    # 2. top-left origin unchanged (extrusion grows away from body)
    assert abs(ex0 - fx0) <= 2 and abs(ey0 - fy0) <= 2, "extrude origin should stay near flat"

    # 3. depth scaling: depth 40 reaches further than depth 20
    deep = render("ex-deep", "{" + POS + BS + "extrude(40,45)}" + TEXT)
    dx0, dy0, dx1, dy1 = bbox(deep)
    assert dx1 > ex1 + 8 and dy1 > ey1 + 8, "depth 40 should extend past depth 20"

    # 4. direction: angle 90 (straight down) barely extends right but far down;
    #    angle 0 (straight right) the opposite
    down = render("ex-down", "{" + POS + BS + "extrude(20,90)}" + TEXT)
    right = render("ex-right", "{" + POS + BS + "extrude(20,0)}" + TEXT)
    dwx0, dwy0, dwx1, dwy1 = bbox(down)
    rx0, ry0, rx1, ry1 = bbox(right)
    assert (dwy1 - fy1) > (dwx1 - fx1) + 8, "angle 90 should extend mostly downward"
    assert (rx1 - fx1) > (ry1 - fy1) + 8, "angle 0 should extend mostly rightward"

    # 5. \extrude0 disables
    off = render("ex-off", "{" + POS + BS + "extrude(20,45)" + BS + "extrude0}" + TEXT)
    ox0, oy0, ox1, oy1 = bbox(off)
    assert abs(ox1 - fx1) <= 1 and abs(oy1 - fy1) <= 1, "extrude0 should match flat"

    # 6. works without shadow depth (the POS block sets \shad0): ink exists far
    #    from the body only because of extrusion copies
    assert len(ext["nonzero"]) > len(flat["nonzero"]) * 1.5, "extrude should add ink mass"

    # 7. determinism
    again = render("ex-basic2", "{" + POS + BS + "extrude(20,45)}" + TEXT)
    assert len(again["nonzero"]) == len(ext["nonzero"]), "determinism"

    print("extrude tests passed")


run()
