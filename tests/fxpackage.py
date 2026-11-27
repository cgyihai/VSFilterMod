#!/usr/bin/env python3
"""Regression checks for the effect package: \\glowc \\glowa \\iglow \\mblur \\bevel \\clipf."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render  # noqa: E402


def inkbox(frame):
    xs = [p[0] for p in frame["nonzero"]]
    ys = [p[1] for p in frame["nonzero"]]
    return (min(xs), min(ys), max(xs), max(ys))


def alpha_at(frame, x, y):
    for px, py, rgba in frame["nonzero"]:
        if px == x and py == y:
            return rgba[3]
    return 0


def main():
    if len(sys.argv) != 3:
        print("usage: fxpackage.py <vsfmrender> <workdir>")
        return 2

    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)
    bs = chr(92)

    def T(tags, text="ABCabc"):
        return "{" + bs + "pos(320,180)" + tags + "}" + text

    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    # ---- \glowc: the glow takes the override colour, not the text colour
    glowc = render(renderer, fontdir, workdir, "fx-glowc", T(bs + "glow20" + bs + "glowc&HFF0000&"))
    plain = render(renderer, fontdir, workdir, "fx-glowplain", T(bs + "glow20"))
    # sample a pixel well outside the glyph ink but inside the halo: halo of 'A' left side
    def halo_pixel(fr):
        # find the leftmost glowing pixel row middle and step further out
        box = inkbox(fr)
        ymid = (box[1] + box[3]) // 2
        for x in range(box[0], box[2]):
            for px, py, rgba in fr["nonzero"]:
                if px == x and py == ymid and rgba[3] > 30:
                    return rgba
        return None
    hp = halo_pixel(glowc)
    pp = halo_pixel(plain)
    check(hp is not None and hp[2] > hp[0], "glowc halo should be blue-dominant (BGR: b>r expected inverse)")
    check(pp is not None, "plain halo exists")

    # glowc syntax: parenthesized with &H equals suffix
    glowc2 = render(renderer, fontdir, workdir, "fx-glowc2", T(bs + "glow20" + bs + "glowc(&HFF0000&)"))
    check(glowc["hash"] == glowc2["hash"], "glowc suffix vs parens mismatch")

    # ---- \glowa: high transparency weakens the glow
    glowa = render(renderer, fontdir, workdir, "fx-glowa", T(bs + "glow20" + bs + "glowa&HC0&"))
    check(glowa["alpha_sum"] < plain["alpha_sum"] - 1000, "glowa should reduce glow alpha")
    check(glowa["alpha_sum"] > 1000, "glowa should not erase everything")
    glowa0 = render(renderer, fontdir, workdir, "fx-glowa0", T(bs + "glow20" + bs + "glowa&H00&"))
    check(glowa0["hash"] == plain["hash"], "glowa 0 should equal plain glow")

    # ---- \iglow: syntax equivalence, disabling, colour visibility
    ig8 = render(renderer, fontdir, workdir, "fx-ig8", T(bs + "iglow8"))
    ig8p = render(renderer, fontdir, workdir, "fx-ig8p", T(bs + "iglow(8)"))
    check(ig8["hash"] == ig8p["hash"], "iglow8 vs iglow(8) mismatch")
    base = render(renderer, fontdir, workdir, "fx-base", T(""))
    ig0 = render(renderer, fontdir, workdir, "fx-ig0", T(bs + "iglow0"))
    check(ig0["hash"] == base["hash"], "iglow0 should equal base")
    check(ig8["hash"] != base["hash"], "iglow8 should differ from base")
    igc = render(renderer, fontdir, workdir, "fx-igc", T(bs + "iglow4" + bs + "iglowc&H0000FF&"))
    # red inner glow: interior pixels should shift red-wards relative to white base;
    # compare a solid interior pixel: use the centroid of the ink
    box = inkbox(igc)
    cxm = (box[0] + box[2]) // 2
    cym = (box[1] + box[3]) // 2
    found = None
    for px, py, rgba in igc["nonzero"]:
        if abs(px - cxm) < 30 and abs(py - cym) < 5 and rgba[3] > 200 and rgba[0] > rgba[2] + 20:
            found = rgba
            break
    check(found is not None, "iglowc red should produce red-shifted interior pixels")

    # ---- \mblur: widening along the blur direction, disabling, angle
    mb = render(renderer, fontdir, workdir, "fx-mb", T(bs + "mblur20"))
    mb0 = render(renderer, fontdir, workdir, "fx-mb0", T(bs + "mblur0"))
    check(mb0["hash"] == base["hash"], "mblur0 should equal base")
    bw0 = inkbox(base)[2] - inkbox(base)[0]
    bw = inkbox(mb)[2] - inkbox(mb)[0]
    check(bw >= bw0 + 10, "horizontal mblur should widen the ink box (%d vs %d)" % (bw, bw0))
    bh0 = inkbox(base)[3] - inkbox(base)[1]
    bh = inkbox(mb)[3] - inkbox(mb)[1]
    check(bh <= bh0 + 6, "horizontal mblur should not extend much vertically (%d vs %d)" % (bh, bh0))
    mbv = render(renderer, fontdir, workdir, "fx-mbv", T(bs + "mblur(20,90)"))
    bhv = inkbox(mbv)[3] - inkbox(mbv)[1]
    check(bhv >= bh0 + 10, "vertical mblur should extend the ink box vertically (%d vs %d)" % (bhv, bh0))
    mbp = render(renderer, fontdir, workdir, "fx-mbp", T(bs + "mblur(20)"))
    check(mb["hash"] == mbp["hash"], "mblur20 vs mblur(20) mismatch")

    # ---- \bevel: changes interior colours, preserves silhouette, disabling
    bv = render(renderer, fontdir, workdir, "fx-bv", T(bs + "bevel2"))
    bv0 = render(renderer, fontdir, workdir, "fx-bv0", T(bs + "bevel0"))
    check(bv0["hash"] == base["hash"], "bevel0 should equal base")
    check(bv["hash"] != base["hash"], "bevel2 should differ from base")
    check(inkbox(bv) == inkbox(base), "bevel should not change the ink box")
    # lighting: some pixels brighter, some darker than the flat fill
    bvs = sorted((rgba for px, py, rgba in bv["nonzero"] if rgba[3] > 200), key=lambda c: c[0])
    bright = sum(1 for c in bvs if c[0] > 200 and c[1] > 200)
    dark = sum(1 for c in bvs if c[0] < 100)
    check(bright > 50 and dark > 20, "bevel should produce both highlights and shadows (%d/%d)" % (bright, dark))

    # ---- \clipf rectangular: the boundary fades instead of cutting
    wide = "ABCabcABCabc"
    cnf = render(renderer, fontdir, workdir, "fx-cnf", "{" + bs + "pos(320,180)" + bs + "clip(200,60,400,300)}" + wide)
    cf = render(renderer, fontdir, workdir, "fx-cf", "{" + bs + "pos(320,180)" + bs + "clip(200,60,400,300)" + bs + "clipf40}" + wide)
    # beyond the nominal right edge x=400 the feathered render still carries ink,
    # the hard one does not
    hard_outside = sum(1 for px, py, rgba in cnf["nonzero"] if px >= 405)
    soft_outside = sum(1 for px, py, rgba in cf["nonzero"] if px >= 405)
    check(hard_outside == 0, "hard clip should have no ink beyond x=405")
    check(soft_outside > 100, "feathered clip should feather beyond x=405 (%d)" % soft_outside)
    # the feather fades out: alpha at x=404 stronger than at x=430
    def max_alpha_in_column(fr, x):
        best = 0
        for px, py, rgba in fr["nonzero"]:
            if px == x and rgba[3] > best:
                best = rgba[3]
        return best
    a1 = max(max_alpha_in_column(cf, xx) for xx in (402, 403, 404))
    a2 = max(max_alpha_in_column(cf, xx) for xx in (425, 426, 427)) if any(px >= 425 and px < 435 for px, py, r in cf["nonzero"]) else 0
    check(a1 > a2, "feather alpha should fall off outwards (%d vs %d)" % (a1, a2))
    # deep interior unaffected: compare a pixel far from edges
    inside_hard = sorted((px, py, rgba) for px, py, rgba in cnf["nonzero"] if 300 <= px <= 320)
    inside_soft = sorted((px, py, rgba) for px, py, rgba in cf["nonzero"] if 300 <= px <= 320)
    check(len(inside_hard) == len(inside_soft), "feather should not change deep interior coverage")

    # inverse clip feather: visible area is outside the rect, ramp rising outwards
    cif = render(renderer, fontdir, workdir, "fx-cif", "{" + bs + "pos(320,180)" + bs + "iclip(200,60,400,300)" + bs + "clipf40}" + wide)
    cinf = render(renderer, fontdir, workdir, "fx-cinf", "{" + bs + "pos(320,180)" + bs + "iclip(200,60,400,300)}" + wide)
    check(cif["alpha_sum"] < cinf["alpha_sum"], "inverse feather should reduce total alpha near the boundary")
    # nothing inside the rect in both
    inside_i = sum(1 for px, py, rgba in cif["nonzero"] if 205 <= px < 395 and 65 <= py < 295)
    check(inside_i == 0, "inverse feather should keep the rect interior empty (%d)" % inside_i)

    # vector clip feather
    cvf = render(renderer, fontdir, workdir, "fx-cvf", "{" + bs + "pos(320,180)" + bs + "clip(m 200 60 l 400 60 400 300 200 300)" + bs + "clipf30}" + wide)
    cvnf = render(renderer, fontdir, workdir, "fx-cvnf", "{" + bs + "pos(320,180)" + bs + "clip(m 200 60 l 400 60 400 300 200 300)}" + wide)
    hardv_outside = sum(1 for px, py, rgba in cvnf["nonzero"] if px >= 405)
    softv_outside = sum(1 for px, py, rgba in cvf["nonzero"] if px >= 405)
    check(softv_outside > hardv_outside + 50, "vector feather should extend softened ink beyond the edge (%d vs %d)" % (softv_outside, hardv_outside))

    # clipf without a clip is a no-op
    cno = render(renderer, fontdir, workdir, "fx-cno", T(bs + "clipf30"))
    check(cno["hash"] == base["hash"], "clipf without clip should be a no-op")

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("OK: fxpackage (glowc, glowa, iglow, mblur, bevel, clipf) verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
