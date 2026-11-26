#!/usr/bin/env python3
"""Checks the VapourSynth entry point, vsfm.TextSubMod.

Renders the consistency fixtures (tests/consistency/*.ass, with the bundled
fonts embedded) onto blank clips of every supported format and checks:

  - that the output, hashed per fixture and format, matches expected.txt --
    the same file on every platform, as for the consistency test;

  - that the formats agree with one another, which is what makes the
    higher-precision blends trustworthy. On a background that is exact at 8
    bits, the subtitle's luma at 9-16 bits shifted down to 8 bits is exactly
    the 8-bit luma, which is upstream's own arithmetic; chroma and RGB are
    within one 8-bit step of it; Gray and 4:2:2 luma equal 4:2:0 luma;

  - that every fixture draws something, and that the argument checks work.

Needs the vapoursynth Python module (pip install vapoursynth) and numpy.
Exits 77 -- skipped, to meson -- when they are not installed, unless the
environment sets VSFM_REQUIRE_VAPOURSYNTH=1 (as CI's VapourSynth job does).

usage:
    run.py --plugin path/to/vsfiltermod.so --out DIR [--update]
"""

import argparse
import glob
import hashlib
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONSISTENCY = os.path.join(os.path.dirname(HERE), "consistency")
# The consistency test's script puts tests/ on sys.path for its own imports.
_spec = importlib.util.spec_from_file_location("consistency_run",
                                               os.path.join(CONSISTENCY, "run.py"))
_consistency = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_consistency)
embed_font = _consistency.embed_font

EXPECTED = os.path.join(HERE, "expected.txt")
WIDTH, HEIGHT = 1280, 720
FPS = 25
STEP = 5                       # every fifth frame: two per 0.4 s event

try:
    import numpy as np
    import vapoursynth as vs
except ImportError as e:
    if os.environ.get("VSFM_REQUIRE_VAPOURSYNTH") == "1":
        raise
    print("skipped: %s" % e)
    sys.exit(77)

core = vs.core

# Backgrounds that are exact at 8 bits, so that higher depths can be compared
# with 8-bit output after a shift.
YUV8 = (128, 100, 160)
RGB8 = (60, 120, 200)

FORMATS = [
    ("YUV420P8", vs.YUV420P8),
    ("YUV420P10", vs.YUV420P10),
    ("YUV420P16", vs.YUV420P16),
    ("YUV422P8", vs.YUV422P8),
    ("YUV422P12", vs.YUV422P12),
    ("GRAY8", vs.GRAY8),
    ("GRAY16", vs.GRAY16),
    ("RGB24", vs.RGB24),
    ("RGB48", vs.RGB48),
]


def full_scale(v, bits):
    """An 8-bit full-range value at `bits`: 255 maps to the maximum."""
    sh = bits - 8
    return (v << sh) | (v >> (8 - sh)) if sh else v


def background(fmt):
    f = core.get_video_format(fmt)
    sh = f.bits_per_sample - 8
    if f.color_family == vs.RGB:
        return [full_scale(v, f.bits_per_sample) for v in RGB8]
    if f.color_family == vs.GRAY:
        return [YUV8[0] << sh]
    return [v << sh for v in YUV8]


def planes(frame):
    return [np.asarray(frame[p]).copy() for p in range(frame.format.num_planes)]


def render(script, fmt, length):
    clip = core.std.BlankClip(width=WIDTH, height=HEIGHT, format=fmt, length=length,
                              fpsnum=FPS, fpsden=1, color=background(fmt))
    sub = core.vsfm.TextSubMod(clip, script)
    return [planes(sub.get_frame(n)) for n in range(0, length, STEP)]


def digest(frames):
    h = hashlib.sha256()
    for frame in frames:
        for p in frame:
            h.update(np.ascontiguousarray(p).astype("<u2" if p.dtype != np.uint8 else np.uint8).tobytes())
    return h.hexdigest()


def max_diff(a, b):
    return int(np.max(np.abs(a.astype(np.int32) - b.astype(np.int32))))


def check_invariants(name, out):
    errors = []
    base = out["YUV420P8"]
    blank = planes(core.std.BlankClip(width=WIDTH, height=HEIGHT, format=vs.YUV420P8,
                                      length=1, color=list(YUV8)).get_frame(0))
    if all(max_diff(f[0], blank[0]) == 0 for f in base):
        errors.append("%s: nothing was drawn" % name)

    for fmt, sh in (("YUV420P10", 2), ("YUV420P16", 8)):
        for i, (hi, lo) in enumerate(zip(out[fmt], base)):
            if not np.array_equal(hi[0] >> sh, lo[0]):
                errors.append("%s: %s luma >> %d differs from YUV420P8 at frame %d (max %d)"
                              % (name, fmt, sh, i * STEP, max_diff(hi[0] >> sh, lo[0])))
                break
            for p in (1, 2):
                d = max_diff(hi[p] >> sh, lo[p])
                if d > 1:
                    errors.append("%s: %s chroma plane %d differs from YUV420P8 by %d at frame %d"
                                  % (name, fmt, p, d, i * STEP))
                    break

    for fmt, sh in (("YUV422P8", 0), ("YUV422P12", 4), ("GRAY8", 0), ("GRAY16", 8)):
        for i, (f, lo) in enumerate(zip(out[fmt], base)):
            if not np.array_equal(f[0] >> sh, lo[0]):
                errors.append("%s: %s luma differs from YUV420P8 at frame %d (max %d)"
                              % (name, fmt, i * STEP, max_diff(f[0] >> sh, lo[0])))
                break

    for i, (hi, lo) in enumerate(zip(out["RGB48"], out["RGB24"])):
        d = max(max_diff(hi[p] >> 8, lo[p]) for p in range(3))
        if d > 1:
            errors.append("%s: RGB48 differs from RGB24 by %d at frame %d" % (name, d, i * STEP))
            break
    return errors


def check_arguments(script):
    errors = []
    clip = core.std.BlankClip(width=64, height=64, format=vs.YUV444P8, length=1)
    for args, needle in (
        ((clip, script), "4:2:0 or 4:2:2"),
        ((clip.resize.Point(format=vs.RGBS), script), "integer"),
        ((core.std.BlankClip(width=64, height=64, length=1, format=vs.YUV420P8),
          os.path.join(HERE, "missing.ass")), "can't open"),
    ):
        try:
            core.vsfm.TextSubMod(*args)
            errors.append("TextSubMod accepted %r" % (args[1:],))
        except vs.Error as e:
            if needle not in str(e):
                errors.append("unexpected error for %s: %s" % (needle, e))
    # fps= and vfr= are accepted; accurate= is accepted and changes nothing.
    yuv = core.std.BlankClip(width=WIDTH, height=HEIGHT, format=vs.YUV420P16, length=20)
    a = planes(core.vsfm.TextSubMod(yuv, script, fps=25.0).get_frame(10))
    b = planes(core.vsfm.TextSubMod(yuv, script, fps=25.0, accurate=1).get_frame(10))
    if any(not np.array_equal(x, y) for x, y in zip(a, b)):
        errors.append("accurate=1 changed the output")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    core.std.LoadPlugin(os.path.abspath(args.plugin))

    manifest, errors = [], []
    scripts = [s for s in sorted(glob.glob(os.path.join(CONSISTENCY, "*.ass")))
               if os.path.basename(s) != "names.ass"]
    for script in scripts:
        name = os.path.splitext(os.path.basename(script))[0]
        source = os.path.join(args.out, name + ".ass")
        embed_font(script, source)
        length = 13 * FPS
        out = {fmt_name: render(source, fmt, length) for fmt_name, fmt in FORMATS}
        for fmt_name, _ in FORMATS:
            manifest.append("%s %s %s" % (name, fmt_name, digest(out[fmt_name])))
        errors += check_invariants(name, out)
    errors += check_arguments(os.path.join(args.out, "text.ass"))

    text = "\n".join(manifest) + "\n"
    with open(os.path.join(args.out, "manifest.txt"), "w", newline="\n") as fh:
        fh.write(text)

    for e in errors:
        print("FAIL: " + e)
    if errors:
        return 1

    if args.update:
        with open(EXPECTED, "w", newline="\n") as fh:
            fh.write(text)
        print("wrote %d digests to %s" % (len(manifest), EXPECTED))
        return 0

    with open(EXPECTED, "r") as fh:
        expected = fh.read().replace("\r\n", "\n").splitlines()
    if manifest == expected:
        print("ok: %d fixture/format digests identical to expected.txt" % len(manifest))
        return 0
    for e, g in zip(expected, manifest):
        if e != g:
            print("FAIL: expected %s\n      got      %s" % (e, g))
    return 1


if __name__ == "__main__":
    sys.exit(main())
