VSFilterMod — cross-platform
============================

A port of VSFilterMod's subtitle renderer to Linux, macOS and Windows,
delivered as a **CSRI** plugin that Aegisub can load on all three, plus a
**BDN+XML** renderer for Blu-ray style subtitle output.

The rasterizer and the ASS tag parser are the original VSFilterMod ones, so
the full mod tag set renders exactly as it always did — including `\pngimg`
picture backgrounds, gradient fills, moveable vector clips, jitter, distort,
symbol rotation, Z-coordinates and vertical spacing.

Building and installing: see [BUILD.md](BUILD.md).

    meson setup build && ninja -C build

Outputs
-------

| Target | Purpose |
| --- | --- |
| `vsfiltermod.dll` / `.so` / `.dylib` | CSRI plugin. Drop it where Aegisub looks and select **CSRI/vsfiltermod_textsub** in Preferences. |
| `vsfmrender` | `vsfmrender --size 1920x1080 --fps 23.976 in.ass out.xml` |

Both come from the same sources, so BDN+XML output matches Aegisub's preview
pixel for pixel.

How this differs from upstream VSFilterMod
------------------------------------------

**Glyphs come from FreeType, not GDI.** The Windows build uses it too, so all
three platforms render identically. The engine deliberately emulates GDI where
the layout code depends on it — most importantly, a positive `LOGFONT` height
is a character *cell* height, not an em size, which is the usual reason a
VSFilter script comes out the wrong size under another renderer. Font *lookup*
is still native: DirectWrite, CoreText or fontconfig.

**Embedded fonts work.** `[Fonts]` attachments are handed to FreeType instead
of being installed into the OS with `AddFontMemResourceEx`, so they now work
through CSRI — where they never did before — and on every platform.

**DirectShow is gone.** The filter, its property pages, the systray applet,
the VobSub/DVB/HDMV/CC/USF/SSF/RealText parsers, the DirectX subpicture
backends and the AviSynth/VapourSynth entry points were all removed. None of
them are reachable from Aegisub or from BDN+XML output. If you want VSFilterMod
as a DirectShow filter or a VapourSynth plugin, use upstream.

**Rendering no longer depends on the machine.** Screen DPI is pinned at 96
rather than read from a live device context, and charset-to-codepage mapping
uses a fixed table rather than the host's locale, so the same script renders
the same everywhere.

Known issues
------------

* OpenType fonts used vertically (`@Source Han Sans`) render smaller than
  expected. This is inherited from VSFilter's layout model and is not fixed by
  the FreeType port.

Licence
-------

GPL-2.0-or-later, as upstream. See [LICENSE](LICENSE).
