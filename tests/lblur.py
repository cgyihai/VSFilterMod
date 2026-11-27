# Regression tests for \shadblur(r) / \bordblur(r) per-layer blur (patch m023).
#
# Additional gaussian blur radius for the shadow / outline pass only, same
# units as \blur, additive with it. The word overlay is re-rasterized per
# pass at the pass-specific blur level. Zero disables; \t animation works.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render  # noqa: E402


def layer_stats(frame, pred):
    """Count and peak alpha of pixels matching a colour predicate."""
    n = 0
    peak = 0
    for px, py, rgba in frame["nonzero"]:
        if pred(rgba):
            n += 1
            if rgba[3] > peak:
                peak = rgba[3]
    return n, peak


def main():
    if len(sys.argv) != 3:
        print("usage: lblur.py <vsfmrender> <workdir>")
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

    # white body, red shadow with a big offset so the layers separate
    base_tags = (bs + "pos(320,180)" + bs + "an5" + bs + "shad(8)"
                 + bs + "4c&H0000FF" + bs + "bord(0)")

    def T(tags, text="ABCDEFGH"):
        return "{" + base_tags + tags + "}" + text

    def redish(rgba):
        return rgba[0] > rgba[2] + 30 and rgba[0] > rgba[1] + 30

    flat = render(renderer, fontdir, workdir, "lblur-flat", T(""))
    blurred = render(renderer, fontdir, workdir, "lblur-shad", T(bs + "shadblur(4)"))

    fn, fpeak = layer_stats(flat, redish)
    bn, bpeak = layer_stats(blurred, redish)

    # 1. blurring the shadow spreads red ink over a larger area
    check(bn > fn * 1.5, "blurred shadow pixel count %d should exceed flat %d" % (bn, fn))

    # 2. blurring softens: peak alpha of the red shadow drops
    check(bpeak < fpeak, "blurred peak alpha %d should drop below flat %d" % (bpeak, fpeak))

    # 3. compact and parenthesised forms agree
    compact = render(renderer, fontdir, workdir, "lblur-compact", T(bs + "shadblur4"))
    check(compact["hash"] == blurred["hash"], "shadblur4 should equal shadblur(4)")

    # 4. \shadblur0 disables: bit-identical to no-tag baseline
    off = render(renderer, fontdir, workdir, "lblur-off", T(bs + "shadblur0"))
    check(off["hash"] == flat["hash"], "shadblur0 should be bit-identical to baseline")

    # 5. bordblur affects the border, not the shadow: with red border instead
    bt = (bs + "pos(320,180)" + bs + "an5" + bs + "shad(0)"
          + bs + "bord(2)" + bs + "3c&H0000FF")

    def TB(tags, text="ABCDEFGH"):
        return "{" + bt + tags + "}" + text

    bflat = render(renderer, fontdir, workdir, "lblur-bflat", TB(""))
    bblurred = render(renderer, fontdir, workdir, "lblur-bord", TB(bs + "bordblur(4)"))
    fbn, fbpeak = layer_stats(bflat, redish)
    bbn, bbpeak = layer_stats(bblurred, redish)
    check(bbn > fbn * 1.3, "blurred border pixel count %d should exceed flat %d" % (bbn, fbn))
    check(bbpeak < fbpeak, "blurred border peak %d should drop below %d" % (bbpeak, fbpeak))

    # 6. layer independence: bordblur leaves a zero-offset shadow unaltered,
    # verified indirectly -- shadblur must not change the border render when
    # no shadow is present at all (shad 0 means no shadow pass ink)
    only_bord = render(renderer, fontdir, workdir, "lblur-onlybord", TB(bs + "shadblur(4)"))
    check(only_bord["hash"] == bflat["hash"],
          "shadblur with zero shadow depth should not alter the image")

    # 7. determinism
    again = render(renderer, fontdir, workdir, "lblur-shad2", T(bs + "shadblur(4)"))
    check(again["hash"] == blurred["hash"], "layer blur must be deterministic")

    if failures:
        print("lblur.py: FAILURES:")
        for f in failures:
            print(" -", f)
        return 1
    print("lblur.py: all tests passed")
    return 0


sys.exit(main())
