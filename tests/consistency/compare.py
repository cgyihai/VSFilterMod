#!/usr/bin/env python3
"""Compares the consistency manifests written by run.py on several platforms.

usage: compare.py expected.txt manifests/<platform>/manifest.txt ...

Groups the platforms by identical output and lists, for every event where
they disagree, what each group rendered. Exits non-zero unless every
manifest equals expected.txt.
"""

import os
import sys


def load(path):
    with open(path, "r") as fh:
        return fh.read().replace("\r\n", "\n").splitlines()


def key(line):
    # "<fixture> <in> <out> <x,y> <wxh> <sha256>": the first three fields
    # identify the event, the rest is what was drawn.
    parts = line.split(" ")
    return " ".join(parts[:3]), " ".join(parts[3:])


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    expected = load(argv[1])
    manifests = {}
    for path in argv[2:]:
        name = os.path.basename(os.path.dirname(path)) or path
        manifests[name.replace("consistency-", "")] = load(path)

    if not manifests:
        print("FAIL: no manifests to compare")
        return 1

    # Platforms with identical output, the expected result first.
    groups = {"expected.txt": ["expected.txt"]}
    contents = {"expected.txt": expected}
    for name, lines in sorted(manifests.items()):
        for label, ref in contents.items():
            if lines == ref:
                groups[label].append(name)
                break
        else:
            groups[name] = [name]
            contents[name] = lines

    print("%d platforms compared" % len(manifests))
    for label, members in groups.items():
        shown = [m for m in members if m != "expected.txt"]
        tag = "matches expected.txt" if label == "expected.txt" else "differs"
        print("  %-22s %s" % (tag + ":", ", ".join(shown) or "(none)"))

    if len(groups) == 1:
        print("ok: every platform rendered every event identically")
        return 0

    # Event-level detail.
    tables = {label: dict(key(l) for l in lines) for label, lines in contents.items()}
    events = []
    for lines in contents.values():
        for line in lines:
            k = key(line)[0]
            if k not in events:
                events.append(k)

    differing = 0
    for event in events:
        values = {label: table.get(event, "(missing)") for label, table in tables.items()}
        if len(set(values.values())) > 1:
            differing += 1
            if differing <= 30:
                print("\n" + event)
                for label, value in values.items():
                    print("    %-40s %s" % (", ".join(m for m in groups[label]), value))
    print("\n%d of %d events differ between groups" % (differing, len(events)))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
