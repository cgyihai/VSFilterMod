# Regression test for \rgrad radial gradients.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render  # noqa: E402


def band_stats(frame, x0, x1, y0=None, y1=None):
    """Average red/blue over opaque pixels in an absolute x/y band."""
    rs = bs = n = 0
    for px, py, rgba in frame["nonzero"]:
        if x0 <= px < x1 and rgba[3] > 100:
            if y0 is not None and not (y0 <= py < y1):
                continue
            rs += rgba[0]
            bs += rgba[2]
            n += 1
    if not n:
        return (0, 0, 0)
    return (rs / n, bs / n, n)


def ink_box(frame):
    box = [9999, 9999, 0, 0]
    for px, py, rgba in frame["nonzero"]:
        if rgba[3] > 100:
            box[0] = min(box[0], px); box[1] = min(box[1], py)
            box[2] = max(box[2], px); box[3] = max(box[3], py)
    return box


def radial_stats(frame):
    """Average red/blue of opaque pixels in the inner third vs the outer
    third of the ink box, binning by distance from the box centre."""
    box = ink_box(frame)
    cx = (box[0] + box[2]) / 2.0
    cy = (box[1] + box[3]) / 2.0
    cap = ((box[2] - box[0]) ** 2 + (box[3] - box[1]) ** 2) ** 0.5 / 2.0
    inn = [0.0, 0.0, 0]
    out = [0.0, 0.0, 0]
    for px, py, rgba in frame["nonzero"]:
        if rgba[3] <= 100:
            continue
        d = ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5 / cap
        slot = inn if d < 1.0 / 3.0 else (out if d > 2.0 / 3.0 else None)
        if slot is not None:
            slot[0] += rgba[0]
            slot[1] += rgba[2]
            slot[2] += 1
    for slot in (inn, out):
        if slot[2]:
            slot[0] /= slot[2]
            slot[1] /= slot[2]
    return inn, out


def main():
    if len(sys.argv) != 3:
        print("usage: rgrad.py <vsfmrender> <workdir>")
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

    # vc colours are BGR: centre red (&H0000FF&), edge blue (&HFF0000&).
    vc = bs + "1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)"

    rg1 = render(renderer, fontdir, workdir, "rg-1", T(vc + bs + "rgrad1"))
    bilin = render(renderer, fontdir, workdir, "rg-bilin", T(vc))

    # centre ink red-dominant, outer ink blue-dominant
    inn1, out1 = radial_stats(rg1)
    check(inn1[2] > 20, "rgrad inner bin has ink")
    check(out1[2] > 20, "rgrad outer bin has ink")
    check(inn1[0] > inn1[1] + 15, "rgrad centre should be red-dominant")
    check(out1[1] > out1[0] + 15, "rgrad edge should be blue-dominant")

    # the plain bilinear gradient has no such centre/edge split: its inner
    # and outer bins should both lean the same way (horizontal mix)
    innB, outB = radial_stats(bilin)
    check(not (innB[0] > innB[1] + 15 and outB[1] > outB[0] + 15),
          "plain bilinear should not show a radial centre/edge split")

    # scale 2 doubles the radius cap: edge ink keeps more of the centre
    # colour than at scale 1
    rg2 = render(renderer, fontdir, workdir, "rg-2", T(vc + bs + "rgrad2"))
    inn2, out2 = radial_stats(rg2)
    check(out2[0] > out1[0] + 5,
          "rgrad2 outer ink should be redder than rgrad1 outer ink")

    # cx = 0 moves the centre to the left edge of the box: left band red,
    # right band blue. The comparison stays inside the vertical middle band
    # so the vertical distance cannot dilute the horizontal signal.
    box = ink_box(rg1)
    w = box[2] - box[0]
    h = box[3] - box[1]
    my0, my1 = box[1] + 2 * h // 5, box[1] + 3 * h // 5
    lx0, lx1 = box[0], box[0] + w // 5
    rx0, rx1 = box[2] - w // 5, box[2]
    rgx = render(renderer, fontdir, workdir, "rg-cx0", T(vc + bs + "rgrad(1,0,0.5)"))
    lx = band_stats(rgx, lx0, lx1, my0, my1)
    rx = band_stats(rgx, rx0, rx1, my0, my1)
    check(lx[2] > 10 and rx[2] > 10, "rgrad cx0 bands have ink")
    check(lx[0] > rx[0] + 10, "rgrad cx0 left band should be redder than right")

    # cy = 0 is the TOP of the screen: top band red, bottom band blue.
    # (pins the vertical orientation of the centre UV; the comparison stays
    # inside the horizontal centre column so the wide box cannot dilute it)
    mx0, mx1 = box[0] + 2 * w // 5, box[0] + 3 * w // 5
    ty0, ty1 = box[1], box[1] + h // 5
    by0, by1 = box[3] - h // 5, box[3]
    rgy = render(renderer, fontdir, workdir, "rg-cy0", T(vc + bs + "rgrad(1,0.5,0)"))
    ty = band_stats(rgy, mx0, mx1, ty0, ty1)
    by = band_stats(rgy, mx0, mx1, by0, by1)
    check(ty[2] > 5 and by[2] > 5, "rgrad cy0 bands have ink")
    check(ty[0] > by[0] + 5, "rgrad cy0 top band should be redder than bottom")

    # suffix and parenthesised forms are equivalent
    rg1p = render(renderer, fontdir, workdir, "rg-1p", T(vc + bs + "rgrad(1)"))
    check(rg1["hash"] == rg1p["hash"], "rgrad suffix vs parens mismatch")

    # \rgrad0 disables the radial mode and restores the bilinear mix
    rg0 = render(renderer, fontdir, workdir, "rg-0", T(vc + bs + "rgrad0"))
    check(rg0["hash"] == bilin["hash"], "rgrad0 should restore the bilinear render")

    # the gradient modes are mutually exclusive, the later tag wins
    rgv = render(renderer, fontdir, workdir, "rg-then-vg",
                 T(vc + bs + "rgrad1" + bs + "vgrad0"))
    vg0 = render(renderer, fontdir, workdir, "rg-vg0", T(vc + bs + "vgrad0"))
    check(rgv["hash"] == vg0["hash"], "vgrad after rgrad should win back the linear mode")
    vgr = render(renderer, fontdir, workdir, "vg-then-rg",
                 T(vc + bs + "vgrad0" + bs + "rgrad1"))
    check(vgr["hash"] == rg1["hash"], "rgrad after vgrad should win the radial mode")

    # a bare \rgrad resets to the line style (radial off -> bilinear)
    rgres = render(renderer, fontdir, workdir, "rg-reset",
                   T(vc + bs + "rgrad1" + bs + "rgrad"))
    check(rgres["hash"] == bilin["hash"], "bare rgrad should reset to the bilinear render")

    # \t animation parses and renders ink across the animated frames
    from glow import render_frames
    rgt = render_frames(renderer, fontdir, workdir, "rg-anim",
                        T(vc + bs + "t(" + bs + "rgrad(2))"))
    check(len(rgt) > 1, "rgrad animated render should span multiple frames")
    check(all(len(f["nonzero"]) > 100 for f in rgt),
          "rgrad animated frames all have ink")

    # a plain line without gradients is untouched by the feature
    plain = render(renderer, fontdir, workdir, "rg-plain", T(""))
    plain2 = render(renderer, fontdir, workdir, "rg-plain2", T(""))
    check(plain["hash"] == plain2["hash"], "plain render not deterministic")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("OK: rgrad centre/edge, scale, centre UV orientation, syntax, "
          "mode exclusion and reset verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
