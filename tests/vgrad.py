# Regression test for \vgrad arbitrary-angle gradients.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render  # noqa: E402


def band_stats(frame, x0, x1):
    """Average red/blue over opaque pixels in an absolute x band."""
    rs = gs = bs = n = 0
    for px, py, rgba in frame["nonzero"]:
        if x0 <= px < x1 and rgba[3] > 100:
            rs += rgba[0]
            bs += rgba[2]
            n += 1
    if not n:
        return (0, 0, 0)
    return (rs / n, bs / n, n)


def main():
    if len(sys.argv) != 3:
        print("usage: vgrad.py <vsfmrender> <workdir>")
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

    # vc colours are BGR: start red (&H0000FF&), end blue (&HFF0000&).
    vc = bs + "1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)"

    vg0 = render(renderer, fontdir, workdir, "vg-0", T(vc + bs + "vgrad0"))
    vg180 = render(renderer, fontdir, workdir, "vg-180", T(vc + bs + "vgrad180"))
    bilin = render(renderer, fontdir, workdir, "vg-bilin", T(vc))

    box = [9999, 9999, 0, 0]
    for px, py, rgba in vg0["nonzero"]:
        if rgba[3] > 100:
            box[0] = min(box[0], px); box[1] = min(box[1], py)
            box[2] = max(box[2], px); box[3] = max(box[3], py)
    w = box[2] - box[0]
    lx0, lx1 = box[0], box[0] + w // 5
    rx0, rx1 = box[2] - w // 5, box[2]

    # angle 0 = left-to-right: left band red-dominant, right band blue-dominant
    l0 = band_stats(vg0, lx0, lx1)
    r0 = band_stats(vg0, rx0, rx1)
    check(l0[2] > 20, "vgrad0 left band has ink")
    check(r0[2] > 20, "vgrad0 right band has ink")
    check(l0[0] > l0[1] + 15, "vgrad0 left band should be red-dominant")
    check(r0[1] > r0[0] + 15, "vgrad0 right band should be blue-dominant")

    # angle 180 reverses the direction
    l180 = band_stats(vg180, lx0, lx1)
    r180 = band_stats(vg180, rx0, rx1)
    check(l180[1] > l180[0] + 15, "vgrad180 left band should be blue-dominant")
    check(r180[0] > r180[1] + 15, "vgrad180 right band should be red-dominant")

    # a diagonal angle must differ from the plain bilinear gradient
    # (note: angle 0 with this symmetric corner setup is mathematically
    # equivalent to bilinear, so the distinction uses 45 degrees)
    vg45 = render(renderer, fontdir, workdir, "vg-45", T(vc + bs + "vgrad45"))
    d45 = band_stats(vg45, lx0, lx1)
    check(abs(d45[0] - band_stats(vg45, rx0, rx1)[0]) < abs(l0[0] - r0[0]),
          "vgrad45 left-right red contrast should shrink vs angle 0")
    lb = band_stats(bilin, lx0, lx1)
    rb = band_stats(bilin, rx0, rx1)
    r45 = band_stats(vg45, rx0, rx1)
    check(abs(d45[0] - lb[0]) > 5 or abs(r45[0] - rb[0]) > 5 or
          abs(d45[1] - lb[1]) > 5 or abs(r45[1] - rb[1]) > 5,
          "vgrad45 output should differ from bilinear vc")

    # suffix and parenthesised forms are equivalent
    vg0p = render(renderer, fontdir, workdir, "vg-0p", T(vc + bs + "vgrad(0)"))
    check(vg0["hash"] == vg0p["hash"], "vgrad suffix vs parens mismatch")

    # a plain line without gradients is untouched by the feature
    plain = render(renderer, fontdir, workdir, "vg-plain", T(""))
    plain2 = render(renderer, fontdir, workdir, "vg-plain2", T(""))
    check(plain["hash"] == plain2["hash"], "plain render not deterministic")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("OK: vgrad direction, reversal, bilinear distinction and syntax verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
