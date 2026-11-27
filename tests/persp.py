# Regression tests for \persp(dist) true-perspective camera distance (patch m024).
#
# The legacy frx/fry projection uses a fixed weak camera distance. \persp
# overrides it in output pixels: smaller distances give dramatic
# foreshortening, large distances approach the orthogonal limit cos(theta).
# For a line of width W rotated by theta around its centre the projected
# width follows width/W = cos(t) d^2 / (d^2 - (W/2 sin(t))^2), which these
# tests validate against the renderer.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render  # noqa: E402


def width_of(frame):
    xs = [p[0] for p in frame["nonzero"]]
    return max(xs) - min(xs), min(xs), max(xs)


def main():
    if len(sys.argv) != 3:
        print("usage: persp.py <vsfmrender> <workdir>")
        return 2
    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)
    bs = chr(92)

    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    TEXT = "AAAAAAAAAAAAAAAA"

    def T(tags):
        return "{" + bs + "pos(320,180)" + bs + "an5" + tags + "}" + TEXT

    flat = render(renderer, fontdir, workdir, "persp-flat", T(""))
    W, fx0, fx1 = width_of(flat)

    rot = bs + "fry60"

    # 1. width follows the homographic width law within tolerance
    import math
    cost, sint = math.cos(math.radians(60)), math.sin(math.radians(60))
    for dist in (400, 800):
        f = render(renderer, fontdir, workdir, "persp-d%d" % dist,
                   T(rot + bs + "persp(%d)" % dist))
        w, x0, x1 = width_of(f)
        a = (W / 2.0) * sint
        pred = W * cost * dist * dist / (dist * dist - a * a)
        ratio = w / pred
        check(0.85 < ratio < 1.18,
              "dist %d width %d vs predicted %.0f (ratio %.2f)" % (dist, w, pred, ratio))

    # 2. smaller distance widens the near-end expansion: width ordering
    f300 = render(renderer, fontdir, workdir, "persp-d300", T(rot + bs + "persp(300)"))
    f800 = render(renderer, fontdir, workdir, "persp-d800", T(rot + bs + "persp(800)"))
    w300, _, _ = width_of(f300)
    w800, _, _ = width_of(f800)
    check(w300 > w800 + 20, "dist300 width %d should exceed dist800 width %d" % (w300, w800))

    # 3. large distance approaches the orthogonal limit cos(60)*W
    big = render(renderer, fontdir, workdir, "persp-d5000", T(rot + bs + "persp(5000)"))
    wbig, _, _ = width_of(big)
    orth = W * cost
    check(abs(wbig - orth) < W * 0.12,
          "dist5000 width %d should approach orthogonal %.0f" % (wbig, orth))

    # 4. \persp0 is the legacy projection: bit-identical to no tag
    legacy = render(renderer, fontdir, workdir, "persp-legacy", T(rot))
    off = render(renderer, fontdir, workdir, "persp-off", T(rot + bs + "persp0"))
    check(off["hash"] == legacy["hash"], "persp0 should be bit-identical to legacy")

    # 5. user distance differs from legacy projection
    check(f800["hash"] != legacy["hash"], "persp(800) should differ from legacy projection")

    # 6. determinism
    again = render(renderer, fontdir, workdir, "persp-d800b", T(rot + bs + "persp(800)"))
    check(again["hash"] == f800["hash"], "persp rendering must be deterministic")

    if failures:
        print("persp.py: FAILURES:")
        for x in failures:
            print(" -", x)
        return 1
    print("persp.py: all tests passed")
    return 0


sys.exit(main())
