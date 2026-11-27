#!/usr/bin/env python3
"""Regression checks for the VSFilterMod \rgb override tag."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glow import render, render_frames  # noqa: E402


def channel_centroid(frame, channel):
    """X centroid of one colour channel, weighted by value."""
    total = 0
    weighted = 0.0
    for y in range(frame["height"]):
        row = frame["pixels"][y]
        for x in range(frame["width"]):
            v = row[x * 4 + channel]
            total += v
            weighted += v * x
    return weighted / total if total else 0.0


def main():
    if len(sys.argv) != 3:
        print("usage: rgb.py <vsfmrender> <workdir>")
        return 1

    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)

    try:
        pos = r"{\pos(320,180)"
        base = render(renderer, fontdir, workdir, "rgb-base", pos + r"}RGB test")
        suffix = render(renderer, fontdir, workdir, "rgb-suffix", pos + r"\rgb4}RGB test")
        paren = render(renderer, fontdir, workdir, "rgb-paren", pos + r"\rgb(4)}RGB test")
        off = render(renderer, fontdir, workdir, "rgb-off", pos + r"\rgb0}RGB test")
        neg = render(renderer, fontdir, workdir, "rgb-neg", pos + r"\rgb(-4)}RGB test")
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1

    def fail(message):
        print("FAIL: " + message)
        return 1

    if suffix["hash"] != paren["hash"]:
        return fail("\rgb4 and \rgb(4) must be equivalent")
    if base["hash"] != off["hash"]:
        return fail("\rgb0 must disable the effect")
    if suffix["hash"] == base["hash"]:
        return fail("\rgb4 did not change the rendered image")
    if neg["hash"] == suffix["hash"]:
        return fail("negative distance must flip the shift direction")

    # red moves right, blue moves left: the red centroid sits to the
    # right of the blue centroid by roughly twice the distance.
    rc = channel_centroid(suffix, 2)
    bc = channel_centroid(suffix, 0)
    sep = rc - bc
    if not (-12 < sep < -4):
        return fail("red/blue centroid separation %.2f does not match distance 4" % sep)
    nrc = channel_centroid(neg, 2)
    nbc = channel_centroid(neg, 0)
    nsep = nrc - nbc
    if not (4 < nsep < 12):
        return fail("negative distance did not flip the separation (%.2f)" % nsep)

    print("OK: rgb syntax, disabling, direction and channel shifts verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
