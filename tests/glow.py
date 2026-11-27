#!/usr/bin/env python3
"""Regression checks for the VSFilterMod \\glow override tag."""

import hashlib
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

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
Style: Default,Caladea,56,{primary},&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,5,20,20,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:00.50,Default,,0,0,0,,{text}
"""


def render_frames(renderer, fontdir, workdir, name, text, primary="&H00FFFFFF"):
    ass_path = os.path.join(workdir, name + ".ass")
    out_dir = os.path.join(workdir, name)
    os.makedirs(out_dir, exist_ok=True)
    xml_path = os.path.join(out_dir, name + ".xml")
    with open(ass_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(SCRIPT_TEMPLATE.format(primary=primary, text=text))

    proc = subprocess.run(
        [renderer, "--quiet", "--size", "640x360", "--fps", "25",
         "--fontdir", fontdir, ass_path, xml_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("%s: renderer failed: %s" %
                           (name, proc.stderr.decode("utf-8", "replace")))

    frames = []
    for event in ET.parse(xml_path).getroot().iter("Event"):
        graphic = event.find("Graphic")
        width, height, rows = read_png(os.path.join(out_dir, graphic.text))
        payload = b"".join(rows)
        alpha_sum = sum(row[i] for row in rows for i in range(3, len(row), 4))
        nonzero = []
        for y, row in enumerate(rows):
            for x in range(width):
                if row[x * 4 + 3]:
                    nonzero.append((int(graphic.get("X")) + x,
                                    int(graphic.get("Y")) + y,
                                    tuple(row[x * 4:x * 4 + 4])))
        frames.append({
            "hash": hashlib.sha256(payload).hexdigest(),
            "width": width,
            "height": height,
            "x": int(graphic.get("X")),
            "y": int(graphic.get("Y")),
            "alpha_sum": alpha_sum,
            "pixels": rows,
            "nonzero": nonzero,
        })
    return frames


def render(renderer, fontdir, workdir, name, text, primary="&H00FFFFFF", allow_empty=False):
    frames = render_frames(renderer, fontdir, workdir, name, text, primary)
    if len(frames) != 1:
        if allow_empty and not frames:
            return None
        raise RuntimeError("%s: expected one BDN event, got %d" % (name, len(frames)))
    return frames[0]


def main():
    if len(sys.argv) != 3:
        print("usage: glow.py <vsfmrender> <workdir>")
        return 2

    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)

    try:
        pos = r"{\pos(320,180)"
        base = render(renderer, fontdir, workdir, "glow-base", pos + r"}Glow test")
        suffix = render(renderer, fontdir, workdir, "glow-suffix", pos + r"\glow20}Glow test")
        paren = render(renderer, fontdir, workdir, "glow-paren", pos + r"\glow(20)}Glow test")
        full = render(renderer, fontdir, workdir, "glow-full", pos + r"\glow(20,1)}Glow test")
        off = render(renderer, fontdir, workdir, "glow-off", pos + r"\glow0}Glow test")
        half = render(renderer, fontdir, workdir, "glow-half", pos + r"\glow(20,0.5)}Glow test")
        double = render(renderer, fontdir, workdir, "glow-double", pos + r"\glow(20,2)}Glow test")
        faded = render(renderer, fontdir, workdir, "glow-alpha", pos + r"\alpha&H80&\glow20}Glow test")
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1

    def fail(message):
        print("FAIL: " + message)
        return 1

    if suffix["hash"] != paren["hash"] or suffix["hash"] != full["hash"]:
        return fail("\\glow20, \\glow(20) and \\glow(20,1) must be equivalent")
    if base["hash"] != off["hash"]:
        return fail("\\glow0 must disable the effect")
    if suffix["hash"] == base["hash"]:
        return fail("\\glow20 did not change the rendered image")
    if suffix["width"] <= base["width"] or suffix["height"] <= base["height"]:
        return fail("glow did not expand the rendered bounding box")
    if not (half["alpha_sum"] < suffix["alpha_sum"] < double["alpha_sum"]):
        return fail("strength is not monotonic: half=%d one=%d double=%d" %
                    (half["alpha_sum"], suffix["alpha_sum"], double["alpha_sum"]))
    if not (0 < faded["alpha_sum"] < suffix["alpha_sum"]):
        return fail("subtitle alpha does not affect glow visibility")

    # \t animates the radius: the exported frame sequence starts with the
    # disabled glow and becomes both larger and brighter as it approaches 20.
    animated = render_frames(renderer, fontdir, workdir, "glow-animated",
                             pos + r"\glow0\t(0,400,\glow20)}Glow test")
    if len(animated) < 3:
        return fail("animated glow did not produce a changing frame sequence")
    if not (animated[-1]["alpha_sum"] > animated[0]["alpha_sum"]
            and animated[-1]["width"] > animated[0]["width"]
            and animated[-1]["height"] > animated[0]["height"]):
        return fail("\\t does not animate glow radius")

    # A timed fade is applied to the glow source as well as the text: the
    # exported sequence becomes brighter while \\fad is fading in.
    fading = render_frames(renderer, fontdir, workdir, "glow-fading",
                           pos + r"\fad(400,0)\glow20}Glow test")
    if len(fading) < 3 or not fading[-1]["alpha_sum"] > fading[0]["alpha_sum"]:
        return fail("timed fade does not affect glow visibility")

    # With a red primary colour and a black outline, the outline supplies the
    # glow shape but not its colour: the outermost glow remains red.
    red = render(renderer, fontdir, workdir, "glow-red", pos + r"\glow20}Glow test",
                 primary="&H000000FF")
    edge = [px for x, y, px in red["nonzero"]
            if x < red["x"] + 8 or x >= red["x"] + red["width"] - 8]
    if not edge or any(px[1] or px[2] for px in edge):
        return fail("outer glow does not follow the primary colour")

    # Rectangular clipping is applied to the source before blurring and to the
    # final glow: nothing may leak left of x=320.
    clipped = render(renderer, fontdir, workdir, "glow-clipped",
                     pos + r"\glow20\clip(320,100,440,260)}Glow test")
    if clipped["nonzero"] and min(x for x, y, px in clipped["nonzero"]) < 320:
        return fail("glow leaks outside a rectangular clip")

    # Vector clips use the rasterizer's historical one-row mask sampling. At
    # a horizontal boundary ordinary text leaves the last nominal row empty;
    # the glow pass must follow that same clip instead of reopening the row.
    vector_clipped = render(
        renderer, fontdir, workdir, "glow-vector-clipped",
        pos + r"\glow20\clip(m 320 100 l 440 100 440 180 320 180)}Glow test")
    if any(y >= 179 for x, y, px in vector_clipped["nonzero"]):
        return fail("glow does not follow vector clip mask sampling")

    # A stationary movevc must take the same path as the static vector clip,
    # and a horizontal move must move the clipped glow boundary with it.
    movevc_zero = render(
        renderer, fontdir, workdir, "glow-movevc-zero",
        pos + r"\glow20\clip(m 320 100 l 440 100 440 180 320 180)\movevc(0,0)}Glow test")
    if movevc_zero["hash"] != vector_clipped["hash"]:
        return fail("stationary movevc does not match the static vector clip")
    movevc_shifted = render(
        renderer, fontdir, workdir, "glow-movevc-shifted",
        pos + r"\glow20\clip(m 320 100 l 440 100 440 180 320 180)\movevc(20,0)}Glow test")
    if not movevc_shifted["nonzero"] or min(x for x, y, px in movevc_shifted["nonzero"]) < 340:
        return fail("glow leaks outside a moving vector clip")

    # If every source pixel is outside the clip there must be no output at
    # all. In particular, text just outside a rectangular clip may not cast
    # light into it, and text inside an inverse-clip hole may not cast light
    # out of it. Exercise the rectangular and vector forms of both.
    empty_cases = [
        ("empty-rect", r"{\an6\pos(300,180)\glow50\clip(320,100,500,260)}Glow"),
        ("empty-inverse", r"{\an6\pos(450,180)\glow50\iclip(320,100,500,260)}Glow"),
        ("empty-vector", r"{\an6\pos(300,180)\glow50\clip(m 320 100 l 500 100 500 260 320 260)}Glow"),
        ("empty-ivector", r"{\an6\pos(450,180)\glow50\iclip(m 320 100 l 500 100 500 260 320 260)}Glow"),
    ]
    for name, text in empty_cases:
        empty = render(renderer, fontdir, workdir, name, text, allow_empty=True)
        if empty is not None and empty["nonzero"]:
            return fail("glow source is not clipped before blurring: " + name)

    print("ok   glow syntax, strength, colour and clipping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
