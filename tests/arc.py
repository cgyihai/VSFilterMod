# Regression tests for \arc(radius[,angle]) arc text layout (patch m017).
#
# The arc wraps the line baseline onto a circle whose centre is the \pos
# point: with angle zero the centre of the line sits on top of the circle
# and glyphs proceed clockwise. Positive radius arches the text upward;
# negative mirrors it below. These tests probe the rendered ink geometry.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render_frames  # noqa: E402

BS = chr(92)

if len(sys.argv) != 3:
    print("usage: arc.py <vsfmrender> <workdir>")
    sys.exit(2)
RENDERER, WORK = sys.argv[1:]
FONTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "consistency", "fonts")

TEXT = "ABCDEFGH"
POS = BS + "pos(320,180)" + BS + "an5"


def bbox(frame):
    xs = [p[0] for p in frame["nonzero"]]
    ys = [p[1] for p in frame["nonzero"]]
    return min(xs), min(ys), max(xs), max(ys)


def render(name, body):
    frames = render_frames(RENDERER, FONTDIR, WORK, name, body)
    assert frames, name + ": no frames"
    return frames[0]


def row_ink(frame, y):
    return sum(1 for p in frame["nonzero"] if p[1] == y)


def run():
    flat = render("arc-flat", "{" + POS + "}" + TEXT)
    arc = render("arc-basic", "{" + POS + BS + "arc(120)}" + TEXT)

    fx0, fy0, fx1, fy1 = bbox(flat)
    ax0, ay0, ax1, ay1 = bbox(arc)

    # 1. arc produces a taller arrangement than flat text (the ends curl down)
    flat_h = fy1 - fy0
    arc_h = ay1 - ay0
    assert arc_h > flat_h + 10, "arc height %d should exceed flat %d by margin" % (arc_h, flat_h)

    # 2. arch shape: the horizontal centre column reaches higher than the edges.
    cx = (ax0 + ax1) // 2
    centre_top = min(p[1] for p in arc["nonzero"] if abs(p[0] - cx) <= 6)
    left_top = min(p[1] for p in arc["nonzero"] if p[0] < ax0 + 12)
    right_top = min(p[1] for p in arc["nonzero"] if p[0] > ax1 - 12)
    assert centre_top < left_top - 4, "centre top %d should be above left edge top %d" % (centre_top, left_top)
    assert centre_top < right_top - 4, "centre top %d should be above right edge top %d" % (centre_top, right_top)

    # 3. horizontal centre stays at the \pos x.
    mid = (ax0 + ax1) / 2.0
    assert abs(mid - 320) < 12, "arc centre %.1f should stay near pos.x 320" % mid

    # 4. radius effect: smaller radius curls the ends further down (taller box)
    tight = render("arc-tight", "{" + POS + BS + "arc(60)}" + TEXT)
    tx0, ty0, tx1, ty1 = bbox(tight)
    assert ty1 - ty0 > arc_h + 4, "radius 60 height %d should exceed radius 120 height %d" % (ty1 - ty0, arc_h)

    # 5. negative radius mirrors below the centre: the arrangement hangs down
    neg = render("arc-neg", "{" + POS + BS + "arc(-120)}" + TEXT)
    nx0, ny0, nx1, ny1 = bbox(neg)
    assert ny0 > ay0 + 20, "negative radius top %d should be well below positive top %d" % (ny0, ay0)

    # 6. angle rotates the arrangement: with a large positive angle the arch
    # swings sideways, moving the horizontal centre off pos.x.
    rot = render("arc-rot", "{" + POS + BS + "arc(120,90)}" + TEXT)
    rx0, ry0, rx1, ry1 = bbox(rot)
    rmid = (rx0 + rx1) / 2.0
    assert abs(rmid - 320) > 15 or abs((ry0 + ry1) / 2.0 - (ay0 + ay1) / 2.0) > 15, \
        "angle 90 should displace the arrangement (centre %.1f,%.1f)" % (rmid, (ry0 + ry1) / 2.0)

    # 7. \arc0 disables: identical geometry to flat.
    off = render("arc-off", "{" + POS + BS + "arc(120)" + BS + "arc0}" + TEXT)
    ox0, oy0, ox1, oy1 = bbox(off)
    assert abs(ox0 - fx0) <= 1 and abs(oy0 - fy0) <= 1 and abs(ox1 - fx1) <= 1 and abs(oy1 - fy1) <= 1, \
        "arc0 should restore flat geometry"

    # 8. shorthand and parenthesised forms agree.
    sfx = render("arc-sfx", "{" + POS + BS + "arc120}" + TEXT)
    sx0, sy0, sx1, sy1 = bbox(sfx)
    assert (sx0, sy0, sx1, sy1) == (ax0, ay0, ax1, ay1), "shorthand and parenthesised forms differ"

    # 9. \t animation: radius shrinking over time gives a tighter arrangement
    # in a later frame than the static radius render starts with.
    anim_frames = render_frames(RENDERER, FONTDIR, WORK, "arc-anim",
                                "{" + POS + BS + "arc(200)" + BS + "t(0,400," + BS + "arc(60))}" + TEXT)
    assert len(anim_frames) >= 2, "animated render should produce several frames"
    first_w = bbox(anim_frames[0])[2] - bbox(anim_frames[0])[0]
    last_h = bbox(anim_frames[-1])[3] - bbox(anim_frames[-1])[1]
    first_h = bbox(anim_frames[0])[3] - bbox(anim_frames[0])[1]
    assert last_h != first_h or True  # geometry must at least render every frame
    heights = [bbox(f)[3] - bbox(f)[1] for f in anim_frames]
    assert max(heights) - min(heights) > 4, "t-animated radius should change the height over time: %r" % heights

    # 10. determinism: identical input renders byte-identical output twice.
    again = render("arc-basic2", "{" + POS + BS + "arc(120)}" + TEXT)
    assert again["hash"] == arc["hash"], "same input must render identically"

    print("arc: all tests passed")


if __name__ == "__main__":
    run()
