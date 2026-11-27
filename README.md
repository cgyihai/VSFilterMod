# VSFilterMod — cross-platform

[![CI](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml/badge.svg)](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)

**English** | [简体中文](README.zh-CN.md)

A port of the VSFilterMod subtitle renderer to **Windows, Linux and macOS**.
It ships as one library that is a **CSRI** plugin for Aegisub, an
**AviSynth+** plugin (`TextSubMod` / `MaskSubMod`) and a **VapourSynth**
plugin (`vsfm.TextSubMod`), plus `vsfmrender`, a command line tool that
renders subtitles to **BDN+XML** for Blu-ray authoring.

The ASS parser and the rasterizer are VSFilterMod's own, so the whole mod tag
set renders as it always did: `\pngimg` picture backgrounds, `\glow` text glow, gradients,
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
- **VapourSynth plugin** (API 4) with upstream's `vsfm.TextSubMod`
  signature: 8-bit output as upstream's, 9–16-bit YUV and RGB blended at
  full precision, also 4:2:2 and Gray.
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
| Windows x86_64 | `vsfiltermod-windows-x86_64-msvc` | `vsfiltermod.dll`, `vsfmrender.exe` |
| Windows x86 (32-bit) | `vsfiltermod-windows-x86-msvc` | `vsfiltermod.dll`, `vsfmrender.exe` — for 32-bit Aegisub, AviSynth+ and avs2bdnxml |
| Windows ARM64 | `vsfiltermod-windows-arm64-msvc` | `vsfiltermod.dll`, `vsfmrender.exe` |
| Linux x86_64 | `vsfiltermod-linux-x86_64` | `vsfiltermod.so`, `vsfmrender` |
| Linux aarch64 | `vsfiltermod-linux-aarch64` | `vsfiltermod.so`, `vsfmrender` |
| macOS universal | `vsfiltermod-macos-universal` | `vsfiltermod.dylib`, `vsfmrender` — Apple Silicon and Intel in one |
| macOS Apple Silicon | `vsfiltermod-macos-arm64` | `vsfiltermod.dylib`, `vsfmrender` |
| macOS Intel | `vsfiltermod-macos-x86_64` | `vsfiltermod.dylib`, `vsfmrender` |

Every build has FreeType and libpng linked in, all at the same pinned version,
so they render identically. The Windows builds need no extra DLLs. The macOS
builds need nothing outside macOS and run on macOS 11 or later (Apple
Silicon) and 10.15 or later (Intel). The Linux builds need only fontconfig and
zlib, which every desktop distribution has, and run on glibc 2.35 or later
(Ubuntu 22.04, Debian 12, Fedora 36 and newer). Each release has a
`SHA256SUMS` file.

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

Any avs2bdnxml that loads scripts through AviSynth+ can use it: take the
32-bit (`windows-x86`) build for a 32-bit avs2bdnxml or AviSynth+, the
64-bit one otherwise.

### VapourSynth

The same library is a VapourSynth plugin (API 4, VapourSynth R55 or later),
with the namespace and arguments of upstream's:

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin("vsfiltermod.dll")      # unless it is in the plugin folder
clip = core.vsfm.TextSubMod(clip, "subtitle.ass")
# TextSubMod(clip, file, charset=1, fps=..., vfr="timecodes.txt")
```

Clips may be YUV 4:2:0 or 4:2:2, Gray or RGB, 8 to 16 bits per sample.
YUV420P8 and RGB24 come out exactly as from upstream's filter. Upstream drew
10- and 16-bit clips as two 8-bit halves; here every depth is blended at its
own precision, so `accurate=` is accepted but no longer needed. For 4:4:4,
convert to RGB.

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
| `--fontdir DIR` | Use the fonts in `DIR` (recursively) ahead of installed ones; repeatable |
| `--font-report` | List what every font the script uses resolved to |
| `--strict-fonts` | Exit with status 3 if any font had to be substituted |
| `-q, --quiet` | No progress output |

### Effect package (\glowc, \glowa, \iglow, \mblur, \bevel, \clipf)

Optional companions to \glow plus four new effects; everything supports \t
animation and a zero value disables:

```ass
{\glowc&HBBGGRR&}    glow colour override (default follows primary colour)
{\glowa&HAA&}        glow alpha override (ASS alpha: 00 opaque, FF transparent)
{\iglow8}            inner glow, radius 8: light bleeding inward from glyph edges
{\iglow(8,1.5)}      with strength; \iglowc / \iglowa override colour/alpha (default white)
{\mblur45,10}        directional motion blur, 45 degrees over 10 output pixels
{\bevel2}            smooth bevel/emboss relief of size 2
{\clipf15}           feathered clip edges, radius 15: rect, vector and inverse clips
```

\iglow is composited after the glyph body and restricted to the interior, so
it never spills outside. \mblur samples the finished bitmap with deterministic
integer arithmetic. \clipf softens the line's clip: rectangular clips expand
their raster region and fade outside the nominal rectangle, while vector clip
masks are blurred once after parsing so every drawing and glow path inherits
the soft edge. The regression suite tests/fxpackage.py covers the package.

### Gradients (\1vc, \vgrad, \rgrad)

The classic \1vc/\2vc/\3vc/\4vc tags interpolate four corner colours
bilinearly across each word. Two modifiers reshape that interpolation; both
use the first two vc colours as start and end, support \t animation and are
mutually exclusive — the later tag wins:

```ass
{\1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\vgrad45}linear gradient along 45 degrees
{\1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\rgrad1}radial gradient, centred
{\rgrad(1.5,0.5,0.2)}scaled 1.5x, centre at 50% width / 20% height
{\rgrad0}back to the bilinear corner mix
```

\vgrad(angle) projects the glyph box onto a direction in degrees (0 runs left
to right, 90 top to bottom). \rgrad(scale[,cx,cy]) radiates from a centre
point: the first vc colour sits at the centre and blends into the second at
the cap radius, which is the scaled distance from the centre to the farthest
box corner; cx/cy are the centre as a UV fraction of the box in screen
orientation and default to the box centre. The regression suites
tests/vgrad.py and tests/rgrad.py pin the semantics.

### Arc text

`\arc(radius[,angle])` wraps the line baseline onto a circle of `radius`
output pixels centred on the line anchor (the `\pos` point for positioned
lines). At angle zero the centre of the line sits on top of the circle and
glyphs proceed clockwise: short text forms a rainbow arch, long text wraps
around the full circle disc-style. Positive radius puts text outside the
circle, negative inside; `\arc120` is shorthand, `\t` animation is
supported and `\arc0` disables the effect. Text is laid out per character,
so Latin ligatures/kerning are lost (CJK unaffected); `\fsp` spacing still
applies along the arc.

### Track mattes (\matte, \imatte)

A track matte uses one event's silhouette to mask events on a layer above it,
After Effects style. The matte event itself is hidden; events on the target
layer render fully - body, outline, shadow, glow and every post-effect - but
only where the matte is opaque:

```ass
Dialogue: 0,...,{\pos(320,180)}TEXT        visible base layer
Dialogue: 1,...,{\pos(320,180)\matte}TEXT  hidden matte copy, masks layer 2
Dialogue: 2,...,{\pos(320,180)\1vc(...)\rgrad1}WWWWW  gradient shows inside TEXT only
```

- `\matte` / `\matte1` / `\matte(1)` masks events on layer+1; `\matte2`
  reaches layer+2 for sparse layer numbering; `\matte0` disables.
- `\imatte` inverts: the effect shows everywhere except inside the matte.
- Several matte events on one layer union into a single mask.
- The mask is the matte's rendered silhouette (shadow, outline and body
  included); a blur or glow on the matte softens the mask edge accordingly.
- Everything animates per frame as usual, so moving or \t-transformed mattes
  and effects stay in sync.

The regression suite tests/matte.py pins containment, inversion, layer
offsets, hiding and the syntax variants.

### RGB split (\rgb)

The \rgb tag shifts the red and blue channels of the text in opposite
directions, a chromatic-aberration / glitch effect. These forms are equivalent:

```ass
{\rgb4}Text
{\rgb(4)}Text
```

The distance is measured in output pixels, may be negative to swap the fringe
directions, supports \t animation, is capped at 4096 and disabled by \rgb0.
The split applies to the whole sentence; when words disagree the largest
magnitude wins.

### Text glow (\glow)

The \glow tag draws a soft halo behind the text. These forms are equivalent:

```ass
{\glow20}Text
{\glow(20)}Text
```

A second number is the linear strength multiplier (the default is `1`):

```ass
{\glow(20,1.5)}Stronger glow
{\glow0}Glow disabled
```

The radius is measured in output pixels, like \blur. The glow normally uses
the primary text colour; gradient and image fills keep their own fill colours,
the outline contributes its shape, and the shadow does not glow. Text
transparency, \fade/\fad, rectangular/vector/moving \clip/\iclip, and \t
animation are respected. The radius is limited to 4096 output pixels and the
strength multiplier to 64.

The glow is a bloom-style multi-scale effect: the emitter shape is blurred
at three radii (tight core, mid, wide halo) in a 16-bit linear-light pipeline
and accumulated additively, then compressed with a Reinhard tone curve so the
halo keeps a smooth brightness gradient instead of saturating into a flat
cloud. The strength multiplier acts as exposure: it scales the linear emission
before tone mapping. It is still an alpha-blended raster effect rather than a
true HDR additive reconstruction of After Effects Deep Glow, and separate glow
colour/alpha controls such as \glowc and \glowa are reserved as possible
future extensions and are not implemented yet.

### Same fonts on every machine

A script that names a font the machine does not have gets whatever the
platform substitutes — a different font on Windows, macOS and Linux. Three
ways to rule that out:

- **Embed the fonts** in the script (`[Fonts]`, e.g. Aegisub's *Attachments*).
- **Share a font folder.** Fonts in it are found exactly like embedded ones,
  ahead of installed fonts. `vsfmrender --fontdir DIR`; for Aegisub and
  AviSynth set the `VSFM_FONT_DIRS` environment variable (several folders
  separated by `;` on Windows, `:` elsewhere).
- **Check.** `vsfmrender` names every substituted font after rendering, and
  `--strict-fonts` makes that an error. For Aegisub and AviSynth, set
  `VSFM_FONT_LOG=1` to print each font as it is resolved.

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
pixel.

**Across platforms the output is identical, not merely close.** Given the
same font files, every build — Windows, Linux and macOS, x86-64 and ARM —
produces the same pixels: there is no SIMD-, CPU- or compiler-dependent code
path, and CI renders `tests/consistency/` on every platform and fails if any
pixel differs from the shared `expected.txt`. What can still differ between
machines is which fonts are installed (see *Known issues*).

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
FreeType instead of being installed into the OS. They are found by every name
GDI would find them by — the Windows family name ("Source Han Sans SC
Medium"), the typographic family, the full name, and the localized names of
any language, including GBK/Big5/Shift-JIS-encoded names in older CJK fonts —
and when several attached faces share a family, the one matching the
requested weight and slant is used, as GDI picks it. Named instances of an
attached variable font are listed the way GDI lists them ("Family Black").
A character none of the usual fallback fonts has is looked up in the attached
fonts (and a `--fontdir` folder) before the platform's own fallback, so it
comes out the same on every machine.

**DirectShow is gone.** The DirectShow filter, its property pages, the
VobSub/DVB/HDMV/CC/USF/SSF/RealText parsers and the DirectX subpicture
backends were removed; none is reachable from Aegisub, AviSynth or BDN+XML
output. `VobSub()` is not carried over, in AviSynth or VapourSynth.

**The VapourSynth plugin targets API 4** (upstream's used the retired API 3)
and blends high bit depths properly instead of splitting samples in two.

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
- The Linux and macOS builds render bit-for-bit like the Windows build (CI
  checks this), but when a script names a font the machine does not have,
  each platform substitutes its own. Embed the fonts in the script
  (`[Fonts]`) to rule that out.

## Credits and licence

Based on **VSFilter** by Gabest (Guliverkli) and **VSFilterMod**. The
AviSynth+ C API headers in `include/avisynth` come from the AviSynth+ project;
`include/vapoursynth/VapourSynth4.h` comes from VapourSynth (LGPL 2.1).

Licensed under the [GNU General Public License v2.0 or later](LICENSE), as
upstream.
