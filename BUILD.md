# Building VSFilterMod

VSFilterMod builds two things:

| Target | What it is |
| --- | --- |
| `vsfiltermod.dll` / `.so` / `.dylib` | A **CSRI** subtitle renderer plugin. This is the form Aegisub loads on every platform. |
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

Writes `output.xml` and `output_NNNN.png` beside it. Static lines produce one
event each; animated lines (`\t`, `\move`, `\fad`, karaoke) are sampled per
frame and identical consecutive frames are merged back into a single event.
`--no-animation` samples animated lines once instead, which is much faster and
freezes them.

Run `vsfmrender --help` for the full option list.

## What was removed

This tree is a cut-down VSFilterMod. Everything specific to DirectShow
playback is gone, because neither Aegisub nor the BDN+XML writer uses it:
the DirectShow filter and its property pages, the VobSub/DVB/HDMV/CC/USF/SSF
and RealText parsers, the DirectX subpicture backends, and the
AviSynth/VapourSynth entry points.

What is kept is the part that matters: the full VSFilterMod tag set, including
the mod extensions (`\pngimg` picture backgrounds, gradients, moveable vector
clip, jitter, distort, symbol rotation, Z-coordinates, vertical spacing).
