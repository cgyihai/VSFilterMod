# VSFilterMod — cross-platform

[![CI](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml/badge.svg)](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)

**English** | [简体中文](README.zh-CN.md)

A port of the VSFilterMod subtitle renderer to **Windows, Linux and macOS**.
It ships as one library that is both a **CSRI** plugin for Aegisub and an
**AviSynth+** plugin (`TextSubMod` / `MaskSubMod`), plus `vsfmrender`, a
command line tool that renders subtitles to **BDN+XML** for Blu-ray authoring.

The ASS parser and the rasterizer are VSFilterMod's own, so the whole mod tag
set renders as it always did: `\pngimg` picture backgrounds, gradients,
moveable vector clips, `\rnd` jitter, distort, `\frs` symbol rotation,
Z-coordinates and vertical spacing.

## Contents

- [Features](#features)
- [Downloads](#downloads)
- [Usage](#usage)
- [Building](#building)
- [Fidelity to upstream](#fidelity-to-upstream)
- [How this differs from upstream](#how-this-differs-from-upstream)
- [Known issues](#known-issues)
- [Credits and licence](#credits-and-licence)

## Features

- **One renderer on three platforms.** Glyphs come from FreeType everywhere,
  so Windows, Linux and macOS render the same script the same way.
- **Upstream look, measured.** Where VSFilterMod's layout depends on GDI —
  cell-height font sizes, synthetic bold and italic, `\frs`, `@` vertical
  fonts, glyph fallback — the engine reproduces GDI's behaviour, worked out by
  probing GDI directly and checked pixel by pixel against upstream.
- **Aegisub plugin** (CSRI) for live preview.
- **AviSynth+ plugin** with upstream's `TextSubMod` / `MaskSubMod`
  signatures, so existing scripts and avs2bdnxml keep working.
- **BDN+XML output** through `vsfmrender`, byte-identical to what avs2bdnxml
  produces from this plugin's `MaskSubMod`.
- **Embedded fonts** (`[Fonts]` attachments) work, through CSRI too.
- **Machine-independent output**: DPI and code pages are fixed, not read from
  the host.

## Downloads

Every push is built by GitHub Actions on all platforms. Tagged versions are
published on the [Releases](https://github.com/cgyihai/VSFilterMod/releases) page; builds
of any commit are under the run's *Artifacts* on the
[Actions](https://github.com/cgyihai/VSFilterMod/actions) page.

| Platform | Archive | Contents |
| --- | --- | --- |
| Windows x86_64 | `vsfiltermod-windows-x86_64-msvc` | `vsfiltermod.dll`, `vsfmrender.exe` — self-contained, no extra DLLs |
| Linux x86_64 | `vsfiltermod-linux-x86_64` | `vsfiltermod.so`, `vsfmrender` |
| Linux aarch64 | `vsfiltermod-linux-aarch64` | `vsfiltermod.so`, `vsfmrender` |
| macOS Apple Silicon | `vsfiltermod-macos-arm64` | `vsfiltermod.dylib`, `vsfmrender` |
| macOS Intel | `vsfiltermod-macos-x86_64` | `vsfiltermod.dylib`, `vsfmrender` |

The Linux and macOS builds link against the system's FreeType, libpng and
fontconfig (Linux) — install those from your package manager or Homebrew.

## Usage

### Aegisub

**Windows:** put `vsfiltermod.dll` in the `csri` folder next to
`aegisub.exe`, restart Aegisub, and choose **CSRI/vsfiltermod_textsub** under
*Preferences → Advanced → Video → Subtitle renderer*.

**Linux / macOS:** stock Aegisub does not load CSRI plugins on these
platforms. Apply [`docs/aegisub-csri-posix.patch`](docs/aegisub-csri-posix.patch)
to Aegisub and rebuild it, then:

```bash
mkdir -p ~/.csri
cp vsfiltermod.so ~/.csri/          # vsfiltermod.dylib on macOS
chmod +x ~/.csri/vsfiltermod.so     # the loader skips files without +x
```

See [BUILD.md](BUILD.md#installing-into-aegisub) for details.

### AviSynth+ and avs2bdnxml

The same library is an AviSynth+ plugin:

```avs
LoadPlugin("vsfiltermod.dll")
MaskSubMod("subtitle.ass", 1920, 1080, 23.976, 3000)   # RGBA, for avs2bdnxml
# or, over a clip:
# TextSubMod("subtitle.ass")
```

Any 64-bit avs2bdnxml that loads scripts through AviSynth+ can use it.
32-bit hosts cannot load a 64-bit plugin.

### vsfmrender

Renders a script straight to BDN+XML, without AviSynth:

```bash
vsfmrender --size 1920x1080 --fps 23.976 input.ass output.xml
```

| Option | Meaning |
| --- | --- |
| `-s, --size WxH` | Frame size (default `1920x1080`) |
| `-f, --fps RATE` | `23.976`, `24`, `25`, `29.97`, `50`, `59.94`, or an exact rational |
| `-l, --lang CODE` | ISO 639-2 language code (default `und`) |
| `-t, --title TEXT` | Title recorded in the index |
| `--first N` / `--last N` | Frame range to export |
| `--no-crop` | Full-frame images (avs2bdnxml `-a0`) |
| `-q, --quiet` | No progress output |

## Building

```bash
meson setup build
meson compile -C build
meson test -C build
```

Requirements: Meson ≥ 0.59, Ninja, a C++17 compiler, FreeType ≥ 2.8,
libpng ≥ 1.6, and fontconfig on Linux. On Windows FreeType, libpng and zlib
are fetched as Meson subprojects. Full instructions, options and the CI
matrix: [BUILD.md](BUILD.md).

## Fidelity to upstream

Measured on Windows against upstream VSFilterMod's CSRI plugin: the overlap
(IoU) of the two renders' glyph coverage, per line. 1.000 is pixel-identical.

| Test script | Covers | IoU |
| --- | --- | --- |
| `tests/smoke.ass` | Basic styles and positioning | 1.000 |
| `tests/frs.ass` | `\frs` all round the circle; CJK, `\fsp`, bold, italic, wrapping | 0.977–0.999 |
| `tests/vertical.ass` | `@` vertical fonts; punctuation, pinyin, Greek, fullwidth | 0.967–1.000 |
| `tests/fallback.ass` | CJK and Hangul in Latin fonts | 0.985–0.999 |
| `tests/bold.ass` | Synthetic bold, with italic, `\frs` and fallback | 0.977–1.000 |

Event timing follows `MaskSubMod`'s clock exactly, single-precision rounding
included, so BDN+XML events start and end on the same frames as upstream's.
The remaining differences are antialiasing at glyph edges, a fraction of a
pixel. Linux and macOS run the same code; there, differences come from which
font files are installed.

## How this differs from upstream

**Glyphs come from FreeType, not GDI.** The engine emulates GDI where the
layout code depends on it: a positive `LOGFONT` height is a cell height, not
an em size (the usual reason a VSFilter script comes out the wrong size
elsewhere); under `\frs` GDI changes the font's metrics and spaces, pivots
and measures glyphs by undocumented rules; `@` faces lay CJK glyphs on their
side in their GSUB `vert` forms. All of these are reproduced — see
`src/font/font_engine.cpp`. Font *lookup* stays native: DirectWrite, CoreText
or fontconfig.

**Missing glyphs fall back as GDI's do.** A character the style's face lacks
is taken from the face's Windows SystemLink list, then from SimSun and Malgun
Gothic, then the macOS and Linux CJK faces, then the platform's own fallback.
Sizing, baseline and the kerning GDI applies to such runs follow GDI.

**Synthetic bold follows GDI's rule**, measured point by point, including the
heavier strokes it uses for rotated text.

**Embedded fonts work** through CSRI and on every platform: attachments go to
FreeType instead of being installed into the OS.

**DirectShow is gone.** The DirectShow filter, its property pages, the
VobSub/DVB/HDMV/CC/USF/SSF/RealText parsers and the DirectX subpicture
backends were removed; none is reachable from Aegisub, AviSynth or BDN+XML
output. `VobSub()` and the VapourSynth plugin are not carried over.

**Rendering does not depend on the machine.** DPI is pinned at 96 and
charset-to-code-page mapping uses a fixed table. The one exception is glyph
fallback on Windows, which follows that machine's SystemLink lists, as GDI
does.

## Known issues

- A vertical `@` face that is OpenType-CFF (`@Source Han Sans`) renders in
  that face here. Upstream's GDI cannot open CFF faces vertically and
  silently substitutes a system font (SimSun on a Chinese Windows); copying
  that would make output depend on the Windows locale.
- `@MS Gothic` and `@Malgun Gothic` leave Greek and Cyrillic upright where
  the port lays them on their side; SimSun and SimHei space a `\frs` run of
  kana differently from other faces. Both come from GDI tables that could
  not be recovered.
- A face name GDI does not know, such as `@Arial`, makes GDI substitute
  SimSun for the whole line, Latin included; here the Latin stays in the
  style face.
- The Linux and macOS builds are compiled and smoke-tested by CI but have
  not been compared against upstream pixel by pixel.

## Credits and licence

Based on **VSFilter** by Gabest (Guliverkli) and **VSFilterMod**. The
AviSynth+ C API headers in `include/avisynth` come from the AviSynth+ project.

Licensed under the [GNU General Public License v2.0 or later](LICENSE), as
upstream.
