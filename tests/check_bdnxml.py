#!/usr/bin/env python3
"""Validates a BDN+XML directory produced by vsfmrender.

Exists so the CI smoke render proves something. Merely running vsfmrender and
checking its exit code would pass even if the platform's font lookup returned
nothing and every image came out blank -- which is exactly the failure mode
that differs between fontconfig, CoreText and DirectWrite.

usage: check_bdnxml.py <output.xml> [--min-events N]
"""

import argparse
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zlib


class CheckError(Exception):
    pass


def read_png(path):
    """Returns (width, height, rows) for an 8-bit RGBA PNG.

    Hand-rolled so the check has no third-party dependency on any runner.
    """
    with open(path, "rb") as fh:
        data = fh.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise CheckError("%s: not a PNG" % path)

    pos = 8
    width = height = 0
    bit_depth = colour_type = None
    idat = b""

    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]

        if kind == b"IHDR":
            width, height, bit_depth, colour_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length

    if bit_depth != 8 or colour_type != 6:
        raise CheckError("%s: expected 8-bit RGBA, got depth %s type %s"
                         % (path, bit_depth, colour_type))
    if width == 0 or height == 0:
        raise CheckError("%s: zero-sized image" % path)

    raw = zlib.decompress(idat)
    stride = width * 4
    rows = []
    prev = bytearray(stride)
    pos = 0

    for _ in range(height):
        if pos >= len(raw):
            raise CheckError("%s: truncated image data" % path)
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if len(line) != stride:
            raise CheckError("%s: truncated scanline" % path)

        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if filt == 1:
                line[x] = (line[x] + a) & 0xFF
            elif filt == 2:
                line[x] = (line[x] + b) & 0xFF
            elif filt == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif filt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
            elif filt != 0:
                raise CheckError("%s: unknown filter %d" % (path, filt))

        rows.append(bytes(line))
        prev = line

    return width, height, rows


def opaque_pixels(width, height, rows):
    count = 0
    for y in range(height):
        row = rows[y]
        for x in range(width):
            if row[x * 4 + 3] > 8:
                count += 1
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xml")
    ap.add_argument("--min-events", type=int, default=1)
    args = ap.parse_args()

    if not os.path.isfile(args.xml):
        print("FAIL: %s does not exist" % args.xml)
        return 1

    directory = os.path.dirname(os.path.abspath(args.xml))

    try:
        tree = ET.parse(args.xml)
    except ET.ParseError as exc:
        print("FAIL: %s is not well-formed XML: %s" % (args.xml, exc))
        return 1

    root = tree.getroot()
    if root.tag != "BDN":
        print("FAIL: root element is <%s>, expected <BDN>" % root.tag)
        return 1

    events = root.findall("./Events/Event")
    declared = root.find("./Description/Events")
    if declared is None:
        print("FAIL: no <Description><Events> summary element")
        return 1

    declared_count = int(declared.get("NumberofEvents", "-1"))
    if declared_count != len(events):
        print("FAIL: NumberofEvents says %d but there are %d <Event> elements"
              % (declared_count, len(events)))
        return 1

    if len(events) < args.min_events:
        print("FAIL: only %d events, expected at least %d"
              % (len(events), args.min_events))
        return 1

    total_ink = 0
    checked = 0

    for index, event in enumerate(events):
        graphic = event.find("Graphic")
        if graphic is None:
            print("FAIL: event %d has no <Graphic>" % index)
            return 1

        name = (graphic.text or "").strip()
        png_path = os.path.join(directory, name)
        if not os.path.isfile(png_path):
            print("FAIL: event %d references missing image %s" % (index, name))
            return 1

        want_w = int(graphic.get("Width", "0"))
        want_h = int(graphic.get("Height", "0"))

        try:
            width, height, rows = read_png(png_path)
        except CheckError as exc:
            print("FAIL: %s" % exc)
            return 1

        if (width, height) != (want_w, want_h):
            print("FAIL: %s is %dx%d but the index says %dx%d"
                  % (name, width, height, want_w, want_h))
            return 1

        # Decoding every image is slow; a sample is enough to prove the
        # renderer produced ink rather than a transparent rectangle.
        if checked < 5:
            ink = opaque_pixels(width, height, rows)
            if ink == 0:
                print("FAIL: %s is fully transparent -- font lookup probably "
                      "returned nothing on this platform" % name)
                return 1
            total_ink += ink
            checked += 1

    fmt = root.find("./Description/Format")
    video = fmt.get("VideoFormat") if fmt is not None else "?"
    rate = fmt.get("FrameRate") if fmt is not None else "?"

    print("ok   %s: %d events, %s @ %s, %d opaque pixels across %d sampled images"
          % (os.path.basename(args.xml), len(events), video, rate, total_ink, checked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
