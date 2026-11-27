#!/usr/bin/env python3
"""Checks that unknown ASS styles produce one aggregated diagnostic."""

import os
import subprocess
import sys

SCRIPT = """[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,36,&H00FFFFFF,&H000000FF,&H00202020,&H80000000,0,0,0,0,100,100,0,0,1,1,0,2,20,20,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:00.40,MissingA,,0,0,0,,first missing style
Dialogue: 0,0:00:00.40,0:00:00.80,MissingB,,0,0,0,,second missing style
Dialogue: 0,0:00:00.80,0:00:01.20,MissingA,,0,0,0,,repeated missing style
"""

EXPECTED = 'Unknown subtitle styles were replaced with "Default": MissingA, MissingB'


def main():
    if len(sys.argv) != 3:
        print("usage: unknown_style_warning.py <vsfmrender> <workdir>")
        return 2

    renderer, workdir = sys.argv[1:]
    os.makedirs(workdir, exist_ok=True)
    ass_path = os.path.join(workdir, "unknown-style-warning.ass")
    xml_path = os.path.join(workdir, "unknown-style-warning.xml")

    with open(ass_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(SCRIPT)

    proc = subprocess.run(
        [renderer, "--size", "640x360", "--fps", "25", ass_path, xml_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    stderr = proc.stderr.decode("utf-8", "replace")

    if proc.returncode != 0:
        print("FAIL: vsfmrender exited with %d\n%s" % (proc.returncode, stderr))
        return 1
    if EXPECTED not in stderr:
        print("FAIL: aggregated warning missing; stderr was:\n%s" % stderr)
        return 1
    if stderr.count("MissingA") != 1 or stderr.count("MissingB") != 1:
        print("FAIL: missing style names should be reported exactly once; stderr was:\n%s" % stderr)
        return 1

    print("ok   unknown style warning is aggregated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
