#!/usr/bin/env python3
"""Cross-platform rendering consistency check.

Renders every *.ass fixture in this directory with vsfmrender and records,
for each BDN+XML event, its timing, its placement and a SHA-256 of its
decoded RGBA pixels. The PNG bytes themselves are not compared: different
zlib builds compress the same pixels differently.

Every font in fonts/ is embedded into each script as a [Fonts] attachment
before rendering. Embedded fonts take precedence over installed ones, so
every platform draws from the very same font files and any difference in
the result is a difference in the renderer. (names.ass relies on this to
check how attachments are found: see fonts/make_test_fonts.py.)

Every platform must reproduce expected.txt exactly.

usage:
    run.py --renderer path/to/vsfmrender --out DIR            check
    run.py --renderer path/to/vsfmrender --out DIR --update   rewrite expected.txt

After an intended change in rendering, regenerate expected.txt with --update
and commit it together with the change.
"""

import argparse
import glob
import hashlib
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from check_bdnxml import read_png  # noqa: E402

FONTS = sorted(glob.glob(os.path.join(HERE, "fonts", "*.ttf")))
EXPECTED = os.path.join(HERE, "expected.txt")
SIZE = "1280x720"
FPS = "25"


def ssa_encode_font(data):
    """SSA/ASS [Fonts] encoding: 6 bits per character, offset by 33, three
    bytes to four characters, a short final group, 80 characters a line."""
    chars = []
    for i in range(0, len(data), 3):
        chunk = data[i:i + 3]
        n = len(chunk)
        b = chunk + b"\0" * (3 - n)
        v = (b[0] << 16) | (b[1] << 8) | b[2]
        group = [(v >> 18) & 63, (v >> 12) & 63, (v >> 6) & 63, v & 63]
        chars.extend(chr(c + 33) for c in group[:n + 1])
    text = "".join(chars)
    return [text[i:i + 80] for i in range(0, len(text), 80)]


def embed_font(script_path, out_path):
    with open(script_path, "r", encoding="utf-8-sig") as fh:
        script = fh.read().replace("\r\n", "\n").rstrip("\n")
    lines = [script, "", "[Fonts]"]
    for path in FONTS:
        with open(path, "rb") as fh:
            font = fh.read()
        stem = os.path.splitext(os.path.basename(path))[0]
        lines.append("fontname: %s_0.ttf" % stem)
        lines += ssa_encode_font(font)
    lines.append("")
    # With a BOM, as Aegisub writes scripts: without one the parser reads
    # them as ANSI, as upstream does.
    with open(out_path, "w", encoding="utf-8-sig", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def render(renderer, script, out_dir):
    name = os.path.splitext(os.path.basename(script))[0]
    work = os.path.join(out_dir, name)
    os.makedirs(work, exist_ok=True)
    for stale in glob.glob(os.path.join(work, "*")):
        os.remove(stale)

    embedded = os.path.join(work, name + ".ass")
    embed_font(script, embedded)

    xml_path = os.path.join(work, name + ".xml")
    subprocess.run([renderer, "--quiet", "--size", SIZE, "--fps", FPS,
                    embedded, xml_path], check=True)

    entries = []
    root = ET.parse(xml_path).getroot()
    for event in root.iter("Event"):
        graphic = event.find("Graphic")
        width, height, rows = read_png(os.path.join(work, graphic.text))
        digest = hashlib.sha256(b"".join(rows)).hexdigest()
        entries.append("%s %s %s %s,%s %sx%s %s" % (
            name, event.get("InTC"), event.get("OutTC"),
            graphic.get("X"), graphic.get("Y"), width, height, digest))
    if not entries:
        raise SystemExit("%s: rendered no events" % name)
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--renderer", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    manifest = []
    for script in sorted(glob.glob(os.path.join(HERE, "*.ass"))):
        manifest += render(args.renderer, script, args.out)

    text = "\n".join(manifest) + "\n"
    with open(os.path.join(args.out, "manifest.txt"), "w", newline="\n") as fh:
        fh.write(text)

    if args.update:
        with open(EXPECTED, "w", newline="\n") as fh:
            fh.write(text)
        print("wrote %d events to %s" % (len(manifest), EXPECTED))
        return 0

    with open(EXPECTED, "r") as fh:
        expected = fh.read().replace("\r\n", "\n").splitlines()

    if manifest == expected:
        print("ok: %d events identical to expected.txt" % len(manifest))
        return 0

    exp_set, got_set = set(expected), set(manifest)
    missing = [e for e in expected if e not in got_set]
    extra = [e for e in manifest if e not in exp_set]
    print("FAIL: rendering differs from expected.txt "
          "(%d expected events missing, %d unexpected)" % (len(missing), len(extra)))
    for line in missing[:20]:
        print("  - " + line)
    for line in extra[:20]:
        print("  + " + line)
    print("full manifest: %s" % os.path.join(args.out, "manifest.txt"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
