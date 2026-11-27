# Regression test for \matte/\imatte track mattes.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render  # noqa: E402


def ink_box(frame):
    box = [9999, 9999, -1, -1]
    for px, py, rgba in frame["nonzero"]:
        if rgba[3] > 100:
            box[0] = min(box[0], px); box[1] = min(box[1], py)
            box[2] = max(box[2], px); box[3] = max(box[3], py)
    return box


def ink_count(frame):
    return sum(1 for px, py, rgba in frame["nonzero"] if rgba[3] > 100)


def color_count(frame, rmin=0, rmax=255, gmin=0, gmax=255, bmin=0, bmax=255):
    n = 0
    for px, py, rgba in frame["nonzero"]:
        r, g, b, a = rgba
        if a > 60 and rmin <= r <= rmax and gmin <= g <= gmax and bmin <= b <= bmax:
            n += 1
    return n


def main():
    if len(sys.argv) != 3:
        print("usage: matte.py <vsfmrender> <workdir>")
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
        else:
            print("ok:", label)

    pos = bs + "pos(320,180)"
    nb = bs + "bord0"
    red = bs + "1c&H0000FF&" + nb
    green = bs + "1c&H00FF00&"
    nl = chr(10)

    def compose(matte_tag, matte_layer=1, fx_layer=2, matte_color=""):
        """Build a multi-layer composition injected through the text slot:
        L0 visible narrow white base 'DE', matte layer hidden copy, fx layer
        wide red 'WWWWWWWW'."""
        lines = []
        lines.append("{" + pos + nb + "}DE")
        lines.append("Dialogue: %d,0:00:00.00,0:00:00.50,Default,,0,0,0,,{%s%s%s%s}DE"
                     % (matte_layer, pos, nb, matte_color, matte_tag))
        lines.append("Dialogue: %d,0:00:00.00,0:00:00.50,Default,,0,0,0,,{%s%s}WWWWWWWW"
                     % (fx_layer, pos, red))
        return nl.join(lines)

    # -- basic containment: masked red ink stays inside the narrow base shape
    masked = render(renderer, fontdir, workdir, "mt-basic", compose(bs + "matte"))
    control = render(renderer, fontdir, workdir, "mt-control", compose(""))
    mb, cb = ink_box(masked), ink_box(control)
    mw, cw = mb[2] - mb[0], cb[2] - cb[0]
    check(ink_count(masked) > 50, "masked render has ink")
    check(cw > 200, "control ink is wide (got %d)" % cw)
    check(0 < mw < cw * 0.55, "masked ink much narrower than control (got %d vs %d)" % (mw, cw))

    # the masked interior shows red where the wide effect glyphs intersect the
    # matte silhouette, while the white base correctly survives in the gaps
    nred = color_count(masked, rmin=150, gmax=110, bmax=110)
    check(nred > 100, "masked render shows red effect ink inside the silhouette")
    nwhite = color_count(masked, rmin=150, gmin=150, bmin=150)
    check(nwhite > 100, "white base survives inside the masked composition")

    # -- matte event itself is hidden: no green pixels anywhere
    hidden = render(renderer, fontdir, workdir, "mt-hidden",
                    compose(bs + "matte", matte_color=green))
    ngreen = color_count(hidden, gmin=150, rmax=110, bmax=110)
    check(ngreen == 0, "matte event hidden (green count %d)" % ngreen)

    # -- inverse matte: wide ink, but the hole lets the white base through.
    # Total ink is conserved (white replaces red inside the hole), so assert
    # on the red ink specifically plus the white-hole signature.
    inv = render(renderer, fontdir, workdir, "mt-inv", compose(bs + "imatte"))
    ib = ink_box(inv)
    iw = ib[2] - ib[0]
    check(iw > cw * 0.6, "inverse matte ink stays wide (got %d vs %d)" % (iw, cw))
    def red_in_region(frame, x0, x1):
        n = 0
        for px, py, rgba in frame["nonzero"]:
            r, g, b, a = rgba
            if a > 60 and r > 150 and g < 110 and b < 110 and x0 <= px <= x1:
                n += 1
        return n
    hole_x0, hole_x1 = mb[0], mb[2]
    red_inv = red_in_region(inv, hole_x0, hole_x1)
    red_ctl = red_in_region(control, hole_x0, hole_x1)
    check(red_ctl > 100, "control has red ink in the hole region")
    check(red_inv < red_ctl * 0.7,
          "inverse matte removes red ink inside the hole (%d vs %d)" % (red_inv, red_ctl))
    white_inv = color_count(inv, rmin=150, gmin=150, bmin=150)
    white_ctl = color_count(control, rmin=150, gmin=150, bmin=150)
    check(white_inv > white_ctl + 100,
          "inverse matte hole reveals white base (%d vs %d)" % (white_inv, white_ctl))
    n_inv, n_norm, n_ctl = ink_count(inv), ink_count(masked), ink_count(control)
    check(0.75 * n_ctl < n_inv + n_norm < 1.35 * n_ctl,
          "matte+imatte roughly partition control ink (%d+%d vs %d)" % (n_norm, n_inv, n_ctl))

    # -- layer offset: matte2 on L1 targets L3, not L2
    off_ok = render(renderer, fontdir, workdir, "mt-off3",
                    compose(bs + "matte2", matte_layer=1, fx_layer=3))
    ow = ink_box(off_ok)[2] - ink_box(off_ok)[0]
    check(0 < ow < cw * 0.55, "matte2 masks layer+2 (got %d)" % ow)
    off_no = render(renderer, fontdir, workdir, "mt-off2",
                    compose(bs + "matte2", matte_layer=1, fx_layer=2))
    ow2 = ink_box(off_no)[2] - ink_box(off_no)[0]
    check(ow2 > cw * 0.6, "matte2 does not mask layer+1 (got %d)" % ow2)

    # -- matte0 disables
    dis = render(renderer, fontdir, workdir, "mt-zero", compose(bs + "matte0"))
    dw = ink_box(dis)[2] - ink_box(dis)[0]
    check(dw > cw * 0.6, "matte0 disables masking (got %d)" % dw)

    # -- syntax equivalence: bare, suffix, parenthesised
    h1 = render(renderer, fontdir, workdir, "mt-syn1", compose(bs + "matte"))["hash"]
    h2 = render(renderer, fontdir, workdir, "mt-syn2", compose(bs + "matte1"))["hash"]
    h3 = render(renderer, fontdir, workdir, "mt-syn3", compose(bs + "matte(1)"))["hash"]
    check(h1 == h2 == h3, "bare/suffix/parenthesised matte forms identical")

    # -- determinism
    d1 = render(renderer, fontdir, workdir, "mt-det1", compose(bs + "matte"))["hash"]
    d2 = render(renderer, fontdir, workdir, "mt-det2", compose(bs + "matte"))["hash"]
    check(d1 == d2, "deterministic rendering")

    if failures:
        print("FAIL:")
        for f in failures:
            print(" -", f)
        return 1
    print("matte: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
