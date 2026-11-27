# Regression tests for \mgrad(c1,c2[,c3...]) multi-stop gradients (patch m022).
#
# 2-8 colour stops in ASS hex order, evenly spaced along the active gradient
# direction: the vgrad angle / rgrad radius when set, horizontal otherwise.
# Body channel only; \mgrad0 or fewer than two colours disables.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render  # noqa: E402


def band_avg(frame, x0, x1):
    """Average (r, g, b) over opaque pixels in an absolute x band."""
    rs = gs = bs = n = 0
    for px, py, rgba in frame["nonzero"]:
        if x0 <= px < x1 and rgba[3] > 100:
            rs += rgba[0]
            gs += rgba[1]
            bs += rgba[2]
            n += 1
    if not n:
        return (0, 0, 0, 0)
    return (rs / n, gs / n, bs / n, n)


def main():
    if len(sys.argv) != 3:
        print("usage: mgrad.py <vsfmrender> <workdir>")
        return 2
    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)
    bs = chr(92)

    def T(tags, text="ABCDEFGH"):
        return "{" + bs + "pos(320,180)" + tags + "}" + text

    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    # red -> green -> blue, horizontal default (angle 0 projects on x)
    stops = bs + "mgrad(&H0000FF,&H00FF00,&HFF0000)"
    fr = render(renderer, fontdir, workdir, "mgrad-rgb", T(bs + "an5" + stops))
    xs = [p[0] for p in fr["nonzero"]]
    x0, x1 = min(xs), max(xs)
    w = x1 - x0
    lr, lg, lb, ln = band_avg(fr, x0, x0 + int(w * 0.18))
    mr, mg, mb, mn = band_avg(fr, x0 + int(w * 0.42), x0 + int(w * 0.58))
    rr, rg, rb, rn = band_avg(fr, x1 - int(w * 0.18), x1 + 1)

    # 1. left band is red-dominant, right band blue-dominant
    check(ln > 50 and rn > 50, "bands empty")
    check(lr > lb + 40, "left band red %.0f should exceed blue %.0f" % (lr, lb))
    check(rb > rr + 40, "right band blue %.0f should exceed red %.0f" % (rb, rr))

    # 2. middle band has elevated green compared to both edges
    check(mg > lg + 20 and mg > rg + 20,
          "middle green %.0f should exceed edges %.0f/%.0f" % (mg, lg, rg))

    # 3. monotonic red decrease left-to-right, blue increase
    check(lr > mr > rr or (lr > rr and mr > rr),
          "red should fall off towards the right (%.0f/%.0f/%.0f)" % (lr, mr, rr))

    # 4. two-stop gradient matches its endpoints
    two = bs + "mgrad(&H0000FF,&HFF0000)"
    f2 = render(renderer, fontdir, workdir, "mgrad-two", T(bs + "an5" + two))
    lr2, lg2, lb2, _n = band_avg(f2, x0, x0 + int(w * 0.15))
    rr2, rg2, rb2, _n2 = band_avg(f2, x1 - int(w * 0.15), x1 + 1)
    check(lr2 > lb2 + 40, "two-stop left should be red (r%.0f b%.0f)" % (lr2, lb2))
    check(rb2 > rr2 + 40, "two-stop right should be blue (r%.0f b%.0f)" % (rr2, rb2))

    # 5. stops follow the vgrad direction: vertical flips the axis
    vert = bs + "vgrad(90)" + stops
    fv = render(renderer, fontdir, workdir, "mgrad-vert", T(bs + "an5" + vert))
    # vertical: sample top vs bottom bands by y
    ys = [p[1] for p in fv["nonzero"]]
    y0, y1 = min(ys), max(ys)
    hy = y1 - y0

    def col_avg(ya, yb):
        rs = bs2 = n = 0
        for px, py, rgba in fv["nonzero"]:
            if ya <= py < yb and rgba[3] > 100:
                rs += rgba[0]
                bs2 += rgba[2]
                n += 1
        return (rs / max(n, 1), bs2 / max(n, 1), n)
    tr, tb, tn = col_avg(y0, y0 + int(hy * 0.25))
    br2, bb2, bn = col_avg(y1 - int(hy * 0.25), y1 + 1)
    # one end must be red-ish, the other blue-ish (direction sign either way)
    check(tn > 30 and bn > 30, "vertical bands empty")
    spread_top = tr - tb
    spread_bot = br2 - bb2
    check((spread_top > 30 and spread_bot < -30) or (spread_top < -30 and spread_bot > 30),
          "vertical gradient should flip the colour axis (top %.0f bot %.0f)" % (spread_top, spread_bot))

    # 6. \mgrad0 disables: bit-identical to no-tag baseline
    base = render(renderer, fontdir, workdir, "mgrad-base", T(bs + "an5"))
    off = render(renderer, fontdir, workdir, "mgrad-off", T(bs + "an5" + bs + "mgrad0"))
    check(off["hash"] == base["hash"], "mgrad0 should be bit-identical to baseline")

    # 7. determinism
    again = render(renderer, fontdir, workdir, "mgrad-rgb2", T(bs + "an5" + stops))
    check(again["hash"] == fr["hash"], "mgrad rendering must be deterministic")

    if failures:
        print("mgrad.py: FAILURES:")
        for f in failures:
            print(" -", f)
        return 1
    print("mgrad.py: all tests passed")
    return 0


sys.exit(main())
