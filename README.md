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
VSFilter script comes out the wrong size under another renderer. The same
goes for `\frs`: under a rotated `lfOrientation` GDI changes the font's
metrics and spaces, pivots and measures the glyphs by rules it does not
document, and the engine reproduces those (worked out by probing GDI; see
`src/font/font_engine.cpp`). Vertical `@` faces are laid out as GDI lays
them on a horizontal line too: CJK glyphs on their side, in their GSUB
`vert` forms, placed by their vertical metrics. Font *lookup* is still
native: DirectWrite, CoreText or fontconfig.

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

* A vertical `@` face that is OpenType-CFF (`@Source Han Sans`, `@Adobe Fan
  Heiti Std`) renders in that face here, laid out vertically. Upstream's GDI
  cannot open CFF faces vertically and silently substitutes a system font —
  SimSun on a Chinese Windows — so those lines differ. Copying the substitute
  would make output depend on the Windows locale. TrueType `@` faces (SimSun,
  Microsoft YaHei, MS Gothic, …) match upstream.
* `@MS Gothic` and `@Malgun Gothic` leave Greek and Cyrillic upright where
  the port, following the Chinese faces, lays them on their side; and SimSun
  and SimHei space a `\frs` run of kana differently from the other faces.
  Both come from GDI tables that could not be recovered.

Licence
-------

GPL-2.0-or-later, as upstream. See [LICENSE](LICENSE).
