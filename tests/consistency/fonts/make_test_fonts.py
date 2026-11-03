#!/usr/bin/env python3
"""Generates the VSFMTest-*.ttf fonts used by tests/consistency/names.ass.

They exist to check that a script's [Fonts] attachments are found by every
name GDI would find them by, and that the right face of a family is chosen.
Each face draws A, B and C as different geometric shapes, so a wrong choice
-- or a fallback to an installed font -- changes the rendered pixels.

    VSFMTest-Regular  family "VSFM Test", also "测试字体" (zh-CN), and
                      "旧式名称" stored GBK-encoded (Windows platform,
                      PRC encoding) the way old Chinese fonts name themselves;
                      weight 400. A: hollow square, B: triangle up, C: bar.
    VSFMTest-Bold     family "VSFM Test" / "测试字体", subfamily Bold, full
                      name "VSFM Test Bold"; weight 700. A: solid square,
                      B: triangle down, C: upright bar.
    VSFMTest-Heavy    legacy family (name ID 1) "VSFM Test Heavy", which is
                      what GDI lists, typographic family (ID 16) "VSFM Test",
                      which is what FreeType reports; weight 900.
                      A: octagon, B: diamond, C: cross.

The generated files are committed; rerun this only to change them:
    python3 make_test_fonts.py        (needs fontTools)

These fonts are part of VSFilterMod's test suite and are distributed under
the same licence, GPL-2.0-or-later.
"""

import os

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables._n_a_m_e import NameRecord

HERE = os.path.dirname(os.path.abspath(__file__))

UPM = 1000
ADVANCE = 800


def poly(pen, points):
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


def rect(pen, x0, y0, x1, y1):
    poly(pen, [(x0, y0), (x0, y1), (x1, y1), (x1, y0)])          # clockwise


def hole(pen, x0, y0, x1, y1):
    poly(pen, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])          # counter-clockwise


SHAPES = {
    "Regular": {
        "A": lambda p: (rect(p, 100, 0, 700, 600), hole(p, 200, 100, 600, 500)),
        "B": lambda p: poly(p, [(100, 0), (400, 650), (700, 0)]),
        "C": lambda p: rect(p, 100, 250, 700, 350),
    },
    "Bold": {
        "A": lambda p: rect(p, 100, 0, 700, 600),
        "B": lambda p: poly(p, [(100, 650), (700, 650), (400, 0)]),
        "C": lambda p: rect(p, 350, 0, 450, 650),
    },
    "Heavy": {
        "A": lambda p: poly(p, [(250, 0), (100, 150), (100, 450), (250, 600),
                                (550, 600), (700, 450), (700, 150), (550, 0)]),
        "B": lambda p: poly(p, [(400, 0), (100, 325), (400, 650), (700, 325)]),
        "C": lambda p: (rect(p, 100, 275, 700, 375), rect(p, 350, 0, 450, 275),
                        rect(p, 350, 375, 450, 650)),
    },
}


def glyph(draw):
    pen = TTGlyphPen(None)
    if draw:
        draw(pen)
    return pen.glyph()


def build(style, names, weight, bold, out):
    order = [".notdef", "space", "A", "B", "C"]
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({0x20: "space", 0x41: "A", 0x42: "B", 0x43: "C"})
    shapes = SHAPES[style]
    fb.setupGlyf({
        ".notdef": glyph(lambda p: rect(p, 100, 0, 700, 700)),
        "space": glyph(None),
        "A": glyph(shapes["A"]),
        "B": glyph(shapes["B"]),
        "C": glyph(shapes["C"]),
    })
    fb.setupHorizontalMetrics({g: (ADVANCE if g != "space" else 400, 0) for g in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupOS2(usWeightClass=weight, sTypoAscender=800, sTypoDescender=-200,
                sTypoLineGap=0, usWinAscent=800, usWinDescent=200,
                fsSelection=(0x20 if bold else 0x40),
                achVendID="VSFM")
    fb.setupPost()
    fb.setupHead(unitsPerEm=UPM, macStyle=(1 if bold else 0))
    fb.setupNameTable(names["en"])

    table = fb.font["name"]
    for name_id, text in names.get("zh", {}).items():
        table.setName(text, name_id, 3, 1, 0x804)
    for name_id, text in names.get("gbk", {}).items():
        # Windows platform, PRC encoding: each character is one 16-bit unit
        # holding its GBK bytes (ASCII as 0x00XX).
        raw = b""
        for ch in text:
            b = ch.encode("gbk")
            raw += b if len(b) == 2 else b"\x00" + b
        rec = NameRecord()
        rec.nameID, rec.platformID, rec.platEncID, rec.langID = name_id, 3, 3, 0x804
        rec.string = raw
        table.names.append(rec)

    fb.save(os.path.join(HERE, out))


def main():
    build("Regular", {
        "en": {"familyName": "VSFM Test", "styleName": "Regular",
               "fullName": "VSFM Test Regular", "psName": "VSFMTest-Regular",
               "uniqueFontIdentifier": "VSFMTest-Regular", "version": "Version 1.0"},
        "zh": {1: "测试字体", 2: "Regular"},
        "gbk": {1: "旧式名称"},
    }, 400, False, "VSFMTest-Regular.ttf")

    build("Bold", {
        "en": {"familyName": "VSFM Test", "styleName": "Bold",
               "fullName": "VSFM Test Bold", "psName": "VSFMTest-Bold",
               "uniqueFontIdentifier": "VSFMTest-Bold", "version": "Version 1.0"},
        "zh": {1: "测试字体", 2: "Bold"},
    }, 700, True, "VSFMTest-Bold.ttf")

    build("Heavy", {
        "en": {"familyName": "VSFM Test Heavy", "styleName": "Regular",
               "typographicFamily": "VSFM Test", "typographicSubfamily": "Heavy",
               "fullName": "VSFM Test Heavy", "psName": "VSFMTest-Heavy",
               "uniqueFontIdentifier": "VSFMTest-Heavy", "version": "Version 1.0"},
    }, 900, False, "VSFMTest-Heavy.ttf")


if __name__ == "__main__":
    main()
