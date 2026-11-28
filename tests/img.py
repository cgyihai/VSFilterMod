# Regression test for line-level \1img texture coordinates and \imgscale.
import hashlib
import os
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_bdnxml import read_png  # noqa: E402

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


def png_chunk(kind, payload):
    data = kind + payload
    return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data) & 0xffffffff)


def write_rgba_png(path, width, height, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4])
                   for y in range(height))
    payload = (b"\x89PNG\r\n\x1a\n"
               + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
               + png_chunk(b"IDAT", zlib.compress(raw, 6))
               + png_chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(payload)


def write_test_texture(path):
    width, height = 64, 32
    rgba = bytearray()
    for y in range(height):
        for x in range(width):
            rgba += bytes([x * 4, y * 8, 128, 255])
    write_rgba_png(path, width, height, rgba)


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
    nonzero = []
    for y, row in enumerate(rows):
        for x in range(width):
            if row[x * 4 + 3]:
                nonzero.append((int(graphic.get("X")) + x,
                                int(graphic.get("Y")) + y,
                                tuple(row[x * 4:x * 4 + 4])))
    return {
        "hash": hashlib.sha256(b"".join(rows)).hexdigest(),
        "width": width,
        "height": height,
        "x": int(graphic.get("X")),
        "y": int(graphic.get("Y")),
        "pixels": rows,
        "nonzero": nonzero,
    }


def rect_pair_text(tags="", posx=100, posy=100, imgargs="tex.png"):
    bs = chr(92)
    rect = "m 0 0 l 20 0 20 20 0 20"
    return ("{" + bs + "an7" + bs + "pos(%d,%d)" % (posx, posy)
            + bs + "bord0" + bs + "shad0" + bs + "1img(" + imgargs + ")" + tags
            + bs + "p1}" + rect
            + "{" + bs + "p0}"
            + "{" + bs + "p1}" + rect
            + "{" + bs + "p0}")


def red_row(frame, row=0):
    pixels = frame["pixels"][row]
    return [pixels[x * 4] for x in range(frame["width"])]


def green_column(frame, col=0):
    return [frame["pixels"][y][col * 4 + 1] for y in range(frame["height"])]


def absolute_pixels(frame):
    return {(x, y): rgba for x, y, rgba in frame["nonzero"]}


def main():
    if len(sys.argv) != 3:
        print("usage: img.py <vsfmrender> <workdir>")
        return 2

    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)
    write_test_texture(os.path.join(workdir, "tex.png"))
    bs = chr(92)
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    try:
        base = render(renderer, fontdir, workdir, "img-base", rect_pair_text())
        moved = render(renderer, fontdir, workdir, "img-moved", rect_pair_text(posx=140))
        scale2 = render(renderer, fontdir, workdir, "img-scale2", rect_pair_text(tags=bs + "imgscale2"))
        scale2p = render(renderer, fontdir, workdir, "img-scale2p", rect_pair_text(tags=bs + "imgscale(2)"))
        scalehalf = render(renderer, fontdir, workdir, "img-scalehalf", rect_pair_text(tags=bs + "imgscale(0.5)"))
        aniso = render(renderer, fontdir, workdir, "img-aniso", rect_pair_text(tags=bs + "imgscale(2,1)"))
        frac = render(renderer, fontdir, workdir, "img-frac", rect_pair_text(tags=bs + "fscx51"))
        scale0 = render(renderer, fontdir, workdir, "img-scale0", rect_pair_text(tags=bs + "imgscale0"))
        scaleneg = render(renderer, fontdir, workdir, "img-scaleneg", rect_pair_text(tags=bs + "imgscale(-2)"))
        scalebad = render(renderer, fontdir, workdir, "img-scalebad", rect_pair_text(tags=bs + "imgscale(not-a-number)"))
        offset = render(renderer, fontdir, workdir, "img-offset", rect_pair_text(imgargs="tex.png,4,0"))
        clipped = render(renderer, fontdir, workdir, "img-clipped",
                         rect_pair_text(tags=bs + "clip(110,90,130,130)"))
        base2 = render(renderer, fontdir, workdir, "img-base2", rect_pair_text())
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1

    check((base["x"], base["y"], base["width"], base["height"]) == (100, 100, 40, 20),
          "unexpected rectangle-pair geometry")

    reds = red_row(base)
    check(reds[0] == 0 and reds[19] == 76 and reds[20] == 80 and reds[39] == 156,
          "texture is not continuous across the word boundary")
    check(all(reds[x + 1] - reds[x] == 4 for x in range(39)),
          "unscaled horizontal texture slope is not 1:1")

    greens = green_column(base)
    check(greens[0] == 88 and greens[-1] == 240,
          "bottom-left line-anchor vertical mapping mismatch")
    check(base["hash"] == moved["hash"],
          "texture pattern does not move with the line anchor")
    check(base["hash"] == base2["hash"], "image fill is not deterministic")

    reds2 = red_row(scale2)
    check(scale2["hash"] == scale2p["hash"], "imgscale suffix and parenthesised forms differ")
    check(reds2[0] == 0 and reds2[1] == 2 and reds2[20] == 40 and reds2[39] == 78,
          "imgscale2 horizontal scaling mismatch")
    check(all(reds2[x + 1] - reds2[x] == 2 for x in range(39)),
          "imgscale2 horizontal slope is not half rate")

    redsh = red_row(scalehalf)
    check(redsh[0] == 0 and redsh[1] == 8 and redsh[31] == 248 and redsh[32] == 0,
          "imgscale(0.5) magnification or wrap mismatch")

    check(red_row(aniso) == reds2, "imgscale(2,1) does not scale x independently")
    check(green_column(aniso) == greens, "imgscale(2,1) changed y despite sy=1")

    fracreds = red_row(frac)
    check(fracreds[10] == 40 and fracreds[11] == 45 and fracreds[12] == 49,
          "fractional word advance does not keep continuous texture phase")
    check(scale0["hash"] == base["hash"], "imgscale0 must disable scaling")
    check(scaleneg["hash"] == scale2["hash"], "negative imgscale must use its magnitude")
    check(scalebad["hash"] == base["hash"], "invalid imgscale must safely restore 1:1")

    check(red_row(offset)[0] == 16, "legacy xoffset is not honoured")

    base_abs = absolute_pixels(base)
    clip_abs = absolute_pixels(clipped)
    compared = 0
    mismatched = 0
    for pos, rgba in clip_abs.items():
        if rgba[3] > 200 and pos in base_abs and base_abs[pos][3] > 200:
            compared += 1
            if rgba[:3] != base_abs[pos][:3]:
                mismatched += 1
    check(compared > 100 and mismatched == 0,
          "rectangular clipping shifts the line-level texture coordinates")

    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    print("OK: line-level image coordinates, scaling, offsets, clipping and determinism verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
