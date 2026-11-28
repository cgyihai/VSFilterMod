# Regression test for the \kk(charcount[,d1...]) typewriter tag.
# Renders a long event and inspects the collapsed BDN frame runs: each reveal
# step must appear at the centisecond-accurate quantized time with strictly
# increasing ink.
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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
Dialogue: 0,0:00:00.00,0:00:06.00,Default,,0,0,0,,{text}
"""


def render(renderer, fontdir, workdir, name, text, duration="0:00:06.00"):
    ass_path = os.path.join(workdir, name + ".ass")
    out_dir = os.path.join(workdir, name)
    os.makedirs(out_dir, exist_ok=True)
    xml_path = os.path.join(out_dir, name + ".xml")
    body = SCRIPT_TEMPLATE.replace("0:00:06.00", duration).replace("{text}", text)
    with open(ass_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)

    proc = subprocess.run(
        [renderer, "--quiet", "--size", "640x360", "--fps", "25",
         "--fontdir", fontdir, ass_path, xml_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("%s: renderer failed: %s" %
                           (name, proc.stderr.decode("utf-8", "replace")))
    events = []
    for ev in ET.parse(xml_path).getroot().iter("Event"):
        g = ev.find("Graphic")
        events.append({
            "in": ev.get("InTC"),
            "out": ev.get("OutTC"),
            "width": int(g.get("Width")),
        })
    return events


def tc_seconds(tc):
    # HH:MM:SS:FF at 25fps
    hh, mm, ss, ff = tc.split(":")
    return int(hh) * 3600 + int(mm) * 60 + int(ss) + int(ff) / 25.0


def main():
    if len(sys.argv) != 3:
        print("usage: typewriter.py <vsfmrender> <workdir>")
        return 2

    renderer, workdir = sys.argv[1:]
    fontdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "consistency", "fonts")
    os.makedirs(workdir, exist_ok=True)
    bs = chr(92)
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    try:
        head = ("{" + bs + "an7" + bs + "pos(100,100)" + bs + "bord0" + bs + "shad0")
        custom = render(renderer, fontdir, workdir, "kk-custom",
                        head + bs + "kk(4,50,,200)}ABCDEFGH")
        defaults = render(renderer, fontdir, workdir, "kk-defaults",
                          head + bs + "kk(3)}ABCDEFGH")
        plain = render(renderer, fontdir, workdir, "kk-plain",
                       head + "}ABCDEFGH", duration="0:00:02.00")
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1

    # --- \kk(4,50,,200)ABCDEFGH: cumulative centisecond reveals at 0.5 / 1.5 /
    # 3.5 / 4.5 seconds. Frames are 40ms, so the first frame at or after each
    # reveal is x.52s (timecode frame 13). Characters beyond the fourth all
    # pop in with D at 4.5s, giving exactly four distinct frame runs.
    check(len(custom) == 4, "custom: expected 4 collapsed frame runs, got %d" % len(custom))
    if len(custom) == 4:
        starts = [tc_seconds(ev["in"]) for ev in custom]
        for got, want in zip(starts, [0.52, 1.52, 3.52, 4.52]):
            check(abs(got - want) < 0.021,
                  "custom: reveal step at %.2fs, expected %.2fs" % (got, want))
        widths = [ev["width"] for ev in custom]
        check(all(widths[i + 1] > widths[i] for i in range(3)),
              "custom: ink width does not increase monotonically: %s" % widths)
        check(widths[3] > widths[2] * 2,
              "custom: final step must add D plus all remaining characters at once")

    # --- \kk(3) with default 100cs delays: reveals at 1s / 2s / 3s and the
    # third step already carries every remaining character.
    check(len(defaults) == 3, "defaults: expected 3 collapsed frame runs, got %d" % len(defaults))
    if len(defaults) == 3:
        starts = [tc_seconds(ev["in"]) for ev in defaults]
        for got, want in zip(starts, [1.0, 2.0, 3.0]):
            check(abs(got - want) < 0.041,
                  "defaults: reveal step at %.2fs, expected %.2fs" % (got, want))

    # --- no kk: a single static frame run.
    check(len(plain) == 1, "plain: expected one frame run, got %d" % len(plain))

    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    print("OK: typewriter cumulative timing, default delays, empty-slot handling and collapse verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
