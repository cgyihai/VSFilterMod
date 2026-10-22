#!/usr/bin/env python3
"""Compares two raw BGRA surfaces dumped by csri_compare.

Both are converted to what avs2bdnxml would receive from MaskSubMod, using the
exact formula from upstream CMemSubPic::AlphaBlt (MSP_RGBA branch):

    A = 255 - invA
    C = (C_premul << 8) / (256 - invA)          -- truncating, no clamp

so the numbers describe the difference a BDN+XML user would actually see.

usage: compare_raw.py a.raw b.raw width height out_prefix
writes out_prefix_a.png, out_prefix_b.png, out_prefix_diff.png (cropped to ink)
"""

import struct
import sys
import zlib


def load(path, w, h):
    data = open(path, "rb").read()
    assert len(data) == w * h * 4, "%s: size mismatch" % path
    return data


def to_masksub_rgba(raw, w, h):
    """Upstream MaskSubMod conversion. Returns a list of (r,g,b,a) rows."""
    out = bytearray(w * h * 4)
    for i in range(w * h):
        b, g, r, inv = raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3]
        if inv < 0xFF:
            bd = 0x100 - inv
            B = (b << 8) // bd
            G = (g << 8) // bd
            R = (r << 8) // bd
            A = 0xFF - inv
            out[i * 4:i * 4 + 4] = bytes((min(R, 255), min(G, 255), min(B, 255), A))
        # else: stays 0,0,0,0 (Blackness)
    return out


def write_png(path, w, h, rgba):
    def chunk(t, b):
        c = t + b
        return struct.pack(">I", len(b)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def ink_bbox(rgba, w, h):
    xs, ys = [], []
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            if rgba[row + x * 4 + 3]:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def crop(rgba, w, box):
    x0, y0, x1, y1 = box
    cw = x1 - x0
    out = bytearray()
    for y in range(y0, y1):
        out += rgba[(y * w + x0) * 4:(y * w + x1) * 4]
    return cw, y1 - y0, out


def main():
    a_path, b_path, w, h, prefix = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    a = to_masksub_rgba(load(a_path, w, h), w, h)
    b = to_masksub_rgba(load(b_path, w, h), w, h)

    box_a, box_b = ink_bbox(a, w, h), ink_bbox(b, w, h)
    if not box_a and not box_b:
        print("both empty")
        return

    # Union box, padded, for the visual crops.
    boxes = [bx for bx in (box_a, box_b) if bx]
    ub = (max(0, min(bx[0] for bx in boxes) - 4), max(0, min(bx[1] for bx in boxes) - 4),
          min(w, max(bx[2] for bx in boxes) + 4), min(h, max(bx[3] for bx in boxes) + 4))

    ink_a = ink_b = both = diff_px = 0
    alpha_abs = 0
    max_a = 0
    for i in range(w * h):
        aa, ba = a[i * 4 + 3], b[i * 4 + 3]
        if aa: ink_a += 1
        if ba: ink_b += 1
        if aa and ba: both += 1
        d = abs(aa - ba)
        if d or a[i*4:i*4+3] != b[i*4:i*4+3]:
            diff_px += 1
        alpha_abs += d
        max_a = max(max_a, d)

    union = ink_a + ink_b - both
    iou = both / union if union else 1.0

    print("  original ink bbox %s  %d px" % (box_a, ink_a))
    print("  port     ink bbox %s  %d px" % (box_b, ink_b))
    print("  coverage IoU %.4f | differing px %d | alpha |diff| mean %.2f max %d"
          % (iou, diff_px, alpha_abs / max(1, union), max_a))

    cw, ch, ca = crop(a, w, ub)
    _, _, cb = crop(b, w, ub)
    # Composite over mid grey for viewing: straight alpha.
    def over_grey(buf):
        out = bytearray(len(buf))
        for i in range(0, len(buf), 4):
            al = buf[i + 3]
            for k in range(3):
                out[i + k] = (buf[i + k] * al + 96 * (255 - al)) // 255
            out[i + 3] = 255
        return out
    ga, gb = over_grey(ca), over_grey(cb)
    # Stack original above port above a diff map (red = only original,
    # green = only port, white = both), 1px separators.
    diff = bytearray(len(ca))
    for i in range(0, len(ca), 4):
        pa, pb = ca[i + 3] > 32, cb[i + 3] > 32
        if pa and pb:   diff[i:i + 4] = b"\xff\xff\xff\xff"
        elif pa:        diff[i:i + 4] = b"\xff\x30\x30\xff"
        elif pb:        diff[i:i + 4] = b"\x30\xff\x30\xff"
        else:           diff[i:i + 4] = b"\x00\x00\x00\xff"
    sep = b"\xff\xff\x00\xff" * cw
    stacked = bytes(ga) + sep + bytes(gb) + sep + bytes(diff)
    write_png(prefix + "_stack.png", cw, ch * 3 + 2, stacked)


if __name__ == "__main__":
    main()
