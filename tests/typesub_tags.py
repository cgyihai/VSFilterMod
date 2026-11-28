# Validation test for the TypeSub editor tag metadata channel.
#
# Checks three things:
#   1. the generated vsfiltermod-tags.json is byte-identical with the raw
#      string payload embedded in csriapi.cpp (the extraction contract);
#   2. the document is well-formed (version/renderer/tags, required keys,
#      no duplicate names) -- reusing the extractor's own validation;
#   3. end to end: the bytes the built plugin actually returns from
#      csri_typesub_tags_json() match the file, so the on-disk manifest an
#      editor reads can never drift from what the renderer reports.
#
# usage: typesub_tags.py <tags.json> <csriapi.cpp> <plugin-library> <build-root>

import ctypes
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from extract_typesub_tags import extract_payload, validate  # noqa: E402

SKIP = 77  # automake/meson skip convention


def main(argv):
    if len(argv) != 5:
        print("usage: typesub_tags.py <tags.json> <csriapi.cpp> <plugin> <build-root>")
        return 2
    tags_path, cpp_path, plugin_path, build_root = argv[1:]

    with open(tags_path, "rb") as fh:
        file_bytes = fh.read()
    payload = extract_payload(cpp_path)
    assert file_bytes == payload, "tags file is not byte-identical with the cpp payload"

    doc = validate(payload)
    names = [t["name"] for t in doc["tags"]]
    for required in ("1img", "imgscale", "glow"):
        assert required in names, required + " missing from tag metadata"
    print("structure ok: %d tags" % len(names))

    # End-to-end: load the built plugin and compare its exported bytes.
    # Windows builds need the subproject DLL directories on the search path;
    # ELF builds statically link those dependencies into the plugin.
    if hasattr(os, "add_dll_directory"):
        build_root = os.path.abspath(build_root)
        dirs = [build_root]
        subprojects = os.path.join(build_root, "subprojects")
        if os.path.isdir(subprojects):
            dirs += [os.path.join(subprojects, d) for d in sorted(os.listdir(subprojects))]
        for d in dirs:
            if os.path.isdir(d):
                os.add_dll_directory(d)
    try:
        lib = ctypes.CDLL(os.path.abspath(plugin_path))
    except OSError as exc:
        print("SKIP: plugin not loadable in test environment: %s" % exc)
        return SKIP
    fn = lib.csri_typesub_tags_json
    fn.restype = ctypes.c_char_p
    exported = fn()
    assert exported, "csri_typesub_tags_json returned NULL/empty"
    assert exported == file_bytes, (
        "DLL export differs from tags file (%d vs %d bytes)" % (len(exported), len(file_bytes)))

    # The exported document must itself parse as the same JSON.
    assert json.loads(exported.decode("utf-8")) == doc
    print("end-to-end ok: DLL export matches file (%d bytes)" % len(exported))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
