#!/usr/bin/env python3
"""Extract the TypeSub editor tag metadata embedded in csriapi.cpp.

The renderer exports csri_typesub_tags_json() so editors can merge this
build's extension tags into their autocompletion without a synchronous
editor release.  The same JSON document is also emitted as a plain file
(vsfiltermod-tags.json) next to the build outputs so editors that prefer
simple file IO over GetProcAddress/dlsym can read it instead.  Both
representations are byte-identical because this script copies the raw
string literal payload verbatim.

Usage:
    extract_typesub_tags.py <path-to-csriapi.cpp> <output.json>

Exits nonzero (failing the build) when the embedded document is missing,
malformed JSON, or structurally invalid -- a broken editor manifest must
never ship silently.
"""

import json
import re
import sys

REQUIRED_TAG_KEYS = ("name", "signature", "help")


def extract_payload(cpp_path):
    with open(cpp_path, "rb") as fh:
        text = fh.read()

    matches = list(re.finditer(rb'R"json\((.*?)\)json"', text, re.DOTALL))
    if not matches:
        raise RuntimeError("%s: no R\"json(...)json\" literal found" % cpp_path)
    if len(matches) > 1:
        raise RuntimeError("%s: multiple R\"json(...)json\" literals found" % cpp_path)
    payload = matches[0].group(1)

    # C++ translation phase 1 maps every line terminator inside the raw
    # string to a single newline, so the bytes the compiler embeds are the
    # LF-normalized form regardless of the source file's line endings.
    # Normalize here too, keeping the file byte-identical with the export.
    return payload.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def validate(payload):
    try:
        doc = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise RuntimeError("embedded tag metadata is not valid UTF-8 JSON: %s" % exc)

    if doc.get("version") != 1:
        raise RuntimeError("tag metadata version must be 1, got %r" % doc.get("version"))
    if not isinstance(doc.get("renderer"), str) or not doc["renderer"]:
        raise RuntimeError("tag metadata requires a non-empty 'renderer' string")
    tags = doc.get("tags")
    if not isinstance(tags, list) or not tags:
        raise RuntimeError("tag metadata requires a non-empty 'tags' array")

    seen = set()
    for index, tag in enumerate(tags):
        if not isinstance(tag, dict):
            raise RuntimeError("tag #%d is not an object" % index)
        for key in REQUIRED_TAG_KEYS:
            if not isinstance(tag.get(key), str) or not tag[key]:
                raise RuntimeError("tag #%d (%r): missing or empty %r" %
                                   (index, tag.get("name"), key))
        if tag["name"] in seen:
            raise RuntimeError("duplicate tag name %r" % tag["name"])
        seen.add(tag["name"])
    return doc


def main(argv):
    if len(argv) != 3:
        print("usage: extract_typesub_tags.py <csriapi.cpp> <output.json>",
              file=sys.stderr)
        return 2

    payload = extract_payload(argv[1])
    doc = validate(payload)

    # Write the payload verbatim (binary, no newline translation) so the
    # file stays byte-identical with the string the DLL exports.
    with open(argv[2], "wb") as fh:
        fh.write(payload)

    print("vsfiltermod-tags.json: %d tags, version %d" %
          (len(doc["tags"]), doc["version"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
