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
| `vsfiltermod.dll` / `.so` / `.dylib` | One library, two faces: a **CSRI** plugin for Aegisub (select **CSRI/vsfiltermod_textsub**), and an **AviSynth+** plugin providing `TextSubMod` / `MaskSubMod`, so `LoadPlugin` scripts and avs2bdnxml work as with upstream. |
| `vsfmrender` | `vsfmrender --size 1920x1080 --fps 23.976 in.ass out.xml` |

All of them run the same renderer, and `vsfmrender`'s BDN+XML output is
byte-identical to what avs2bdnxml produces from this plugin's `MaskSubMod`.

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
the VobSub/DVB/HDMV/CC/USF/SSF/RealText parsers and the DirectX subpicture
backends were removed; none of them are reachable from Aegisub, AviSynth or
BDN+XML output. The AviSynth entry points were rewritten against AviSynth+'s
C interface, so they work on every platform AviSynth+ runs on. `VobSub()` and
the VapourSynth plugin are not carried over; for those, use upstream.

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
