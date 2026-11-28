# Regression test for the \5img/\6img/\7img image fit modes
# (stretch / cover / contain against the line layout box).
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_bdnxml import read_png  # noqa: E402
from img import write_test_texture  # noqa: E402

SCRIPT_TEMPLATE = """[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360
WrapStyle: 0
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Caladea,56,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,0,0,0,7,20,20,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:00.50,Default,,0,0,0,,{text}
"""


def render(renderer, fontdir, workdir, name, text):
    ass_path = os.path.join(workdir, name + ".ass")
    out_dir = os.path.join(workdir, name)
    os.makedirs(out_dir, exist_ok=True)
    xml_path = os.path.join(out_dir, name + ".xml")
    with open(ass_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(SCRIPT_TEMPLATE.format(text=text))

    proc = subprocess.run(
        [renderer, "--quiet", "--size", "640x360", "--fps", "25",
         "--fontdir", fontdir, ass_path, xml_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("%s: renderer failed: %s" %
                           (name, proc.stderr.decode("utf-8", "replace")))

    events = list(ET.parse(xml_path).getroot().iter("Event"))
    if len(events) != 1:
        raise RuntimeError("%s: expected one BDN event, got %d" % (name, len(events)))
    graphic = events[0].find("Graphic")
    width, height, rows = read_png(os.path.join(out_dir, graphic.text))
    nonzero = sum(1 for row in rows for x in range(width) if row[x * 4 + 3])
    return {
        "width": width,
        "height": height,
        "x": int(graphic.get("X")),
        "y": int(graphic.get("Y")),
        "pixels": rows,
        "nonzero": nonzero,
    }


def rect_pair_text(tag, imgargs="tex.png", posx=100, posy=100, extra=""):
    bs = chr(92)
    rect = "m 0 0 l 20 0 20 20 0 20"
    return ("{" + bs + "an7" + bs + "pos(%d,%d)" % (posx, posy)
            + bs + "bord0" + bs + "shad0" + extra + bs + tag + "(" + imgargs + ")"
            + bs + "p1}" + rect
            + "{" + bs + "p0}"
            + "{" + bs + "p1}" + rect
            + "{" + bs + "p0}")


def square_text(tag, extra="", posx=100, posy=100):
    bs = chr(92)
    rect = "m 0 0 l 20 0 20 20 0 20"
    return ("{" + bs + "an7" + bs + "pos(%d,%d)" % (posx, posy)
            + bs + "bord0" + bs + "shad0" + extra + bs + tag + "(tex.png)"
            + bs + "p1}" + rect + "{" + bs + "p0}")


def red_at(frame, x, y):
    return frame["pixels"][y][x * 4]


def alpha_at(frame, x, y):
    return frame["pixels"][y][x * 4 + 3]


def main():
    if len(sys.argv) != 3:
        print("usage: img567.py <vsfmrender> <workdir>")
        return 2

    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)
    write_test_texture(os.path.join(workdir, "tex.png"))
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    try:
        stretch = render(renderer, fontdir, workdir, "img5-stretch", rect_pair_text("5img"))
        stretchoff = render(renderer, fontdir, workdir, "img5-offset", rect_pair_text("5img", imgargs="tex.png,4,0"))
        cover = render(renderer, fontdir, workdir, "img6-cover", square_text("6img"))
        contain = render(renderer, fontdir, workdir, "img7-contain", square_text("7img"))
        bordered = render(renderer, fontdir, workdir, "img5-border", rect_pair_text("5img", extra=chr(92) + "bord3"))
        wrapped = render(renderer, fontdir, workdir, "img1-wrapped", rect_pair_text("1img"))
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1

    # --- \5img stretch: each 20px rect is its own overlay reference box
    # (including rasterisation padding), so both rects show the identical
    # squeeze and the second rect restarts at texel 0 instead of continuing.
    check((stretch["width"], stretch["height"]) == (40, 20),
          "stretch: unexpected rect-pair geometry")
    check(red_at(stretch, 0, 10) == 0, "stretch: left edge does not start at texel 0")
    check(red_at(stretch, 20, 10) == 0,
          "stretch: second rect does not restart at texel 0 (per-overlay reference)")
    mirror = all(red_at(stretch, x, 10) == red_at(stretch, x + 20, 10) for x in range(20))
    check(mirror, "stretch: identical rects are not textured identically")
    slope_reds = [red_at(stretch, x, 10) for x in range(20)]
    check(all(slope_reds[x + 1] > slope_reds[x] for x in range(19)),
          "stretch: texture does not increase monotonically across the box")
    check(200 <= red_at(stretch, 19, 10) <= 240,
          "stretch: right edge does not reach the texture end (got %d)" % red_at(stretch, 19, 10))
    check(stretch["nonzero"] > 700, "stretch: fill area unexpectedly small")
    # The texture is upright: green (PNG row index) increases downwards.
    stretch_greens = [stretch["pixels"][y][10 * 4 + 1] for y in range(20)]
    check(stretch_greens[0] < stretch_greens[-1], "stretch: texture is vertically flipped")

    # Wrapped \1img keeps its 1:1 slope on the same geometry (mode 2 unaffected).
    check(red_at(wrapped, 5, 10) == 20 and red_at(wrapped, 20, 10) == 80,
          "wrapped 1img regression on the same geometry")

    # --- xoffset still applies in box pixels: the left edge starts several
    # texels into the texture instead of at texel 0.
    r0 = red_at(stretchoff, 0, 10)
    check(36 <= r0 <= 56, "stretch xoffset: left edge is not shifted into the texture (got %d)" % r0)
    check(r0 > red_at(stretch, 0, 10) + 20, "stretch xoffset: offset has no effect")

    # --- \6img cover on a square box with a 64x32 texture: the uniform scale
    # is driven by the vertical constraint, the horizontal overflow crops
    # symmetrically around the centre, and the whole box stays painted.
    check((cover["width"], cover["height"]) == (20, 20), "cover: unexpected square geometry")
    check(56 <= red_at(cover, 0, 10) <= 72,
          "cover: left edge is not center-cropped into the texture (got %d)" % red_at(cover, 0, 10))
    cover_reds = [red_at(cover, x, 10) for x in range(20)]
    check(all(cover_reds[x + 1] > cover_reds[x] for x in range(19)),
          "cover: texture does not increase monotonically across the box")
    check(red_at(cover, 19, 10) < 200, "cover: right edge must be cropped well before the texture end")
    check(cover["nonzero"] > 350, "cover: image must cover the whole box, no margins")
    # symmetric crop: the left and right margins cut equal texture shares
    check(abs((red_at(cover, 0, 10) - 0) - (252 - red_at(cover, 19, 10))) < 24,
          "cover: crop is not horizontally centered")

    # --- \7img contain on the same box: scale = 0.3125 fills the width exactly
    # (64*0.3125 = 20) and centers the 10px-high image vertically; the 5px
    # margins above and below stay transparent, so the exported bitmap crops
    # to the fitted image itself.
    check(contain["width"] == 20, "contain: exact horizontal fit must keep the full width")
    check(9 <= contain["height"] <= 11,
          "contain: fitted image is not 10px high (got %d)" % contain["height"])
    check(contain["x"] == 100, "contain: horizontal fit must stay flush with the box left")
    check(4 <= contain["y"] - 100 <= 7,
          "contain: image is not vertically centered (offset %d)" % (contain["y"] - 100))
    midy = contain["height"] // 2
    mid_alpha = max(alpha_at(contain, x, midy) for x in range(contain["width"]))
    check(mid_alpha > 200, "contain: middle row is not opaque")
    # Horizontal fit spans the full texture width across the box.
    check(red_at(contain, 0, midy) == 0, "contain: left edge does not start at texel 0")
    contain_reds = [red_at(contain, x, midy) for x in range(contain["width"])]
    check(all(contain_reds[x + 1] > contain_reds[x] for x in range(contain["width"] - 1)),
          "contain: texture does not increase monotonically across the box")
    check(200 <= red_at(contain, contain["width"] - 1, midy) <= 240,
          "contain: right edge does not reach near the texture end")

    # --- \5img only textures the channel it targets: with \bord3 the border
    # ring keeps the solid outline colour instead of the texture. The texture
    # has a constant blue channel of 128, the solid border is black, so the
    # blue channel is a crisp discriminator on the mid-left border column.
    bmidy = bordered["height"] // 2
    bring_blue = bordered["pixels"][bmidy][2]  # x = 0, blue channel
    bring_alpha = bordered["pixels"][bmidy][3]
    check(bring_alpha > 100 and bring_blue == 0,
          "border ring textured by 5img (channel leak): blue %d alpha %d" % (bring_blue, bring_alpha))
    cx, cy = bordered["width"] // 2, bordered["height"] // 2
    interior_reds = {red_at(bordered, x, cy) for x in range(6, bordered["width"] - 6)}
    check(len(interior_reds) > 4, "bordered interior is not textured (5img lost)")

    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    print("OK: 5img stretch, 6img cover, 7img contain, offsets, continuity and channel isolation verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
