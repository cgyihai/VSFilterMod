# Regression tests for \echo(count,dx,dy[,falloff]) motion-trail copies (patch m020).
#
# The event renders into an offscreen canvas which is composited into the
# frame once per copy at stepped integer offsets with falloff-scaled alpha,
# then once more at full alpha. These tests probe rendered ink geometry.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render_frames  # noqa: E402

BS = chr(92)

if len(sys.argv) != 3:
    print("usage: echo.py <vsfmrender> <workdir>")
    sys.exit(2)
RENDERER, WORK = sys.argv[1:]
FONTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "consistency", "fonts")

TEXT = "ECHO"
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
    flat = render("echo-flat", "{" + POS + "}" + TEXT)
    echo = render("echo-basic", "{" + POS + BS + "echo(3,6,0)}" + TEXT)

    fx0, fy0, fx1, fy1 = bbox(flat)
    ex0, ey0, ex1, ey1 = bbox(echo)

    # 1. copies extend ink rightward by roughly count*dx = 18 px (faintest copy
    #    may drop below detection, so allow slack)
    assert ex1 > fx1 + 8, "echo right %d should exceed flat right %d" % (ex1, fx1)

    # 2. main copy position unchanged: left/top edges match flat
    assert abs(ex0 - fx0) <= 2 and abs(ey0 - fy0) <= 2, "echo origin should match flat"

    # 3. count scaling: echo(6,6,0) extends further than echo(3,6,0)
    more = render("echo-more", "{" + POS + BS + "echo(6,6,0)}" + TEXT)
    mx0, my0, mx1, my1 = bbox(more)
    assert mx1 > ex1 + 6, "echo6 right %d should exceed echo3 right %d" % (mx1, ex1)

    # 4. vertical direction: echo(3,0,6) extends downward not rightward
    vert = render("echo-vert", "{" + POS + BS + "echo(3,0,6)}" + TEXT)
    vx0, vy0, vx1, vy1 = bbox(vert)
    assert vy1 > fy1 + 8, "vertical echo should extend downward"
    assert abs(vx1 - fx1) <= 3, "vertical echo should not extend rightward"

    # 5. negative direction: echo(3,-6,0) extends leftward
    neg = render("echo-neg", "{" + POS + BS + "echo(3,-6,0)}" + TEXT)
    nx0, ny0, nx1, ny1 = bbox(neg)
    assert nx0 < fx0 - 8, "negative dx echo should extend leftward"

    # 6. \echo0 disables
    off = render("echo-off", "{" + POS + BS + "echo(3,6,0)" + BS + "echo0}" + TEXT)
    ox0, oy0, ox1, oy1 = bbox(off)
    assert abs(ox0 - fx0) <= 1 and abs(ox1 - fx1) <= 1, "echo0 should match flat"

    # 7. ink mass grows (copies add real alpha, even faint)
    assert len(echo["nonzero"]) > len(flat["nonzero"]), "echo should add ink"

    # 8. determinism
    again = render("echo-basic2", "{" + POS + BS + "echo(3,6,0)}" + TEXT)
    assert len(again["nonzero"]) == len(echo["nonzero"]), "determinism"

    print("echo tests passed")


run()
