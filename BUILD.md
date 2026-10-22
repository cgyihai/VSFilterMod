# Building VSFilterMod

**English** | [简体中文](BUILD.zh-CN.md)

VSFilterMod builds two things:

| Target | What it is |
| --- | --- |
| `vsfiltermod.dll` / `.so` / `.dylib` | A **CSRI** subtitle renderer plugin — the form Aegisub loads on every platform — that is also an **AviSynth+** plugin (`TextSubMod` / `MaskSubMod`). |
| `vsfmrender` | A command line tool that renders a script to **BDN+XML** (an index plus one PNG per event). |

Both are built from the same renderer sources, so a subtitle exported to
BDN+XML is pixel-identical to what Aegisub previews.

## Requirements

* Meson ≥ 0.59 and Ninja
* A C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
* FreeType ≥ 2.8 and libpng ≥ 1.6 — taken from the system if present,
  otherwise downloaded as Meson subprojects
* Linux/BSD only: **fontconfig** ≥ 2.11
* macOS and Windows use CoreText and DirectWrite respectively; nothing to install

## Build

```bash
meson setup build
ninja -C build
meson test -C build
```

Useful options:

```bash
meson setup build -Dfont_backend=fontconfig   # override platform default
meson setup build -Dbdnxml=false              # plugin only
meson setup build -Dtests=false
```

### Apple Silicon and ARM Linux

The rasterizer's blend kernels are written against SSE2. On AArch64 they can go
through [sse2neon](https://github.com/DLTcollab/sse2neon), a single header:

```bash
curl -LO https://raw.githubusercontent.com/DLTcollab/sse2neon/master/sse2neon.h
meson setup build -Dcpp_args=-I$PWD
```

Without it the build still works and renders identically — it just uses the
scalar kernels, which are slower. Meson prints which one it picked.

## Installing into Aegisub

### Linux / macOS

Aegisub 3.5.6 does not build CSRI support on these platforms out of the box.
Two things block it, both patched by `docs/aegisub-csri-posix.patch`:

* `meson.build` gates the feature on `host_machine.system() == 'windows'`
* `subprojects/csri/meson.build` never defines `CSRI_PATH`, so the POSIX
  enumerator (`lib/posix/enumerate.c`) does not compile — its first statement
  is `static const char csri_path[] = CSRI_PATH;`

```bash
cd /path/to/Aegisub
git apply /path/to/VSFilterMod/docs/aegisub-csri-posix.patch
meson setup build && ninja -C build
```

Then put the plugin anywhere on the search path. The compiled-in default is
`~/.csri` plus `<prefix>/<libdir>/csri`, and the `CSRI_PATH` environment
variable is appended to it:

```bash
mkdir -p ~/.csri
cp build/vsfiltermod.so ~/.csri/        # or .dylib on macOS
chmod +x ~/.csri/vsfiltermod.so         # see note below
```

> The POSIX enumerator skips any file that fails `access(path, X_OK)`, so the
> plugin **must** have its execute bit set. `meson install` does this; a manual
> `cp` may not.

### Windows

Aegisub's Windows enumerator looks in a `csri` directory next to the
executable:

```
Aegisub\
  aegisub.exe
  csri\
    vsfiltermod.dll
```

### Selecting it

Restart Aegisub, then pick **CSRI/vsfiltermod_textsub** under
*Preferences → Advanced → Video → Subtitle renderer*. If it is not listed, the
plugin was not found or failed to load; on Linux and macOS run Aegisub from a
terminal to see the loader's warnings.

## Continuous integration

`.github/workflows/ci.yml` builds and tests six configurations:

| Job | Toolchain | Font backend | Dependencies |
| --- | --- | --- | --- |
| Linux x86_64 | GCC | fontconfig | apt |
| Linux aarch64 | GCC | fontconfig | apt + sse2neon |
| macOS arm64 (`macos-15`) | Clang | CoreText | Homebrew + sse2neon |
| macOS x86_64 (`macos-15-intel`) | Clang | CoreText | Homebrew |
| Windows x86_64 (MSVC) | MSVC | DirectWrite | Meson subprojects, all static |
| Windows x86_64 (MinGW) | GCC | DirectWrite | MSYS2 |

Each job runs the `csri-abi` test — which loads the plugin the way Aegisub's
loader does and renders through it — then renders `tests/smoke.ass` to BDN+XML
and validates the result with `tests/check_bdnxml.py`. That validator rejects
a fully transparent image, so a platform whose font lookup silently returns
nothing fails the build instead of producing empty subtitles.

Two notes:

* **ARM Linux runners are free for public repositories only.** If this repo is
  private, delete the `ubuntu-24.04-arm` matrix entry, or the job will queue
  forever rather than fail.
* The **MSVC artefact is the redistributable one**: freetype, libpng, zlib and
  the CRT are all linked statically, so `vsfiltermod.dll` is a single file with
  no dependencies beyond `DWrite.dll`, `KERNEL32.dll`, `USER32.dll` and
  `ADVAPI32.dll`. The MinGW artefact links against MSYS2's DLLs and is a build
  check only.

### Releases

Pushing a tag that starts with `v` runs the same matrix and then publishes a
GitHub Release with one archive per platform (the plugin, `vsfmrender`, the
licence and the READMEs):

```bash
git tag v1.0.0
git push origin v1.0.0
```

The MinGW build is not published; it is a build check.

## Verifying the plugin outside Aegisub

`tests/csri_loader_test.c` loads the shared object exactly the way the CSRI
loader does — by path, resolving each entry point by name — then parses a
script from memory, requests Aegisub's pixel format and renders a frame. It is
registered with Meson:

```bash
meson test -C build csri-abi
```

## Rendering BDN+XML

```bash
build/vsfmrender --size 1920x1080 --fps 23.976 input.ass output.xml
```

Writes `output.xml` and `output_NNNN.png` beside it.

The event list follows what avs2bdnxml gets from upstream's `MaskSubMod`:
frames are sampled at `(REFERENCE_TIME)(10000000 * n / fps)` computed in single
precision, as upstream does; identical consecutive frames are merged and empty
frames close an event; and pixels use `MaskSubMod`'s un-premultiply formula.
Checked against avs2bdnxml-rs + upstream VSFilterMod with `bdn-diff`: every
event of `tests/smoke.ass` has the same InTC and OutTC.

`--fps` takes `num/den` or a decimal; 23.976, 29.97 and 59.94 mean their
`/1001` rationals, which is what AviSynth gives `MaskSub` for a clip's
`framerate`. Pass the exact rational if your script uses something else.
Every frame is rendered at its own time, as `MaskSubMod` does: its subtitle
provider reports every line as animated.

What `vsfmrender` does **not** do, that avs2bdnxml does: palette (8-bit PNG)
output, splitting an event into two graphics, PGS `.sup` output, and
even-Y enforcement. It writes one RGBA graphic per event.

Run `vsfmrender --help` for the full option list.

## AviSynth+

The same library is an AviSynth+ C plugin:

```
LoadPlugin("vsfiltermod.dll")
MaskSubMod("subs.ass", width=1920, height=1080, fps=24000.0/1001.0, length=34000)
# or, over video:
TextSubMod(clip, "subs.ass", charset=1, fps=-1, vfr="timecodes.txt")
```

`TextSub`, `MaskSub` and `TextSubModSwapUV` are registered too, with upstream's
signatures. Clips may be RGB32, RGB24, YUY2, YV12 or I420; YUV output uses
the matrix from the script's `YCbCr Matrix` header, as upstream did.

The render path mirrors upstream's `CFilter::Render` step for step -- the
single-precision timestamp, the in-memory subpicture cleared over its last
dirty rectangle, compositing only the rendered bounding box, flipping RGB32
and RGB24 but not `MaskSubMod`'s RGBA -- because that is what decides the
pixels avs2bdnxml receives. Checked against upstream through avs2bdnxml-rs
and `tests/avs_dump.c`: identical event timing, identical subtitle rows for
MaskSubMod's RGBA and for TextSubMod on RGB32, RGB24, YUY2 and YV12 (I420
shares the YV12 code path but was not run separately), identical VFR frame
selection.

AviSynth is not a link-time dependency. The plugin resolves the C API from
the AviSynth module that loaded it, so the same file still loads as a CSRI
plugin on a machine with no AviSynth. Build with `-Davisynth=false` to leave
the entry points out.

## Comparing against upstream

`tests/csri_compare.c` renders a script through any CSRI plugin and dumps the
raw surfaces; `tests/compare_raw.py` converts two such dumps with `MaskSubMod`'s
formula and reports coverage IoU, alpha differences and a stacked visual diff.
Point the first at Aegisub's `csri/VSFilterMod.dll` and the second at this
build. Give upstream a script **with a UTF-8 BOM**: through CSRI it throws an
unhandled C++ exception on a BOM-less file.

`tests/frs.ass` and `tests/vertical.ass` (one line per second, sample at
`n.5`) exercise the two places where the engine reproduces undocumented GDI
layout: `\frs` at angles all round the circle, with CJK, `\fsp`, bold,
italic, scaling and wrapping, and `@` vertical faces with punctuation,
pinyin, Greek and fullwidth forms. They use SimSun, SimHei, Microsoft
YaHei, DengXian, MS Gothic, Yu Gothic, Malgun Gothic and Times New Roman,
so run them where those are installed. Against upstream they measured IoU
0.977–0.999 and 0.967–1.000.

`tests/fallback.ass` covers glyph fallback: CJK and Hangul in Arial, Times
New Roman, Tahoma and Segoe UI, with italic, rules, `\fsp`, `\frz`, `\frs`
and an SRT-like default style. Against upstream on a Chinese Windows 11 all
eleven lines measured IoU 0.985–0.999 (0.12–0.67 before glyph fallback).
`tests/bold.ass` covers synthetic bold on SimHei, SimSun and KaiTi, with
italic and `\frs`, and bold fallback glyphs turned by `\frs`: 0.977–1.000
(0.93–0.95 before).

## What was removed

This tree is a cut-down VSFilterMod. Everything specific to DirectShow
playback is gone, because neither Aegisub nor the BDN+XML writer uses it:
the DirectShow filter and its property pages, the VobSub/DVB/HDMV/CC/USF/SSF
and RealText parsers, the DirectX subpicture backends, `VobSub()`, and the
VapourSynth plugin.

What is kept is the part that matters: the full VSFilterMod tag set, including
the mod extensions (`\pngimg` picture backgrounds, gradients, moveable vector
clip, jitter, distort, symbol rotation, Z-coordinates, vertical spacing).
