/*
 *  VSFilterMod cross-platform port -- FreeType glyph engine.
 *
 *  This replaces the CreateFontIndirect / SelectObject / GetTextExtentPoint32W
 *  / BeginPath+TextOutW+EndPath / GetPath sequence that RTS.cpp used to drive
 *  through a screen-compatible HDC.
 *
 *  It deliberately emulates GDI rather than doing "the FreeType thing",
 *  because the whole layout engine above it is calibrated against GDI:
 *
 *    * A positive LOGFONT::lfHeight is a *cell* height (tmAscent + tmDescent),
 *      not an em size. RTS asks for fontSize * 64 as a positive height, so
 *      getting this wrong scales every subtitle on screen. The em size is
 *      derived as lfHeight * unitsPerEm / (usWinAscent + usWinDescent).
 *    * GDI paths are y-down with the origin at the top of the character cell,
 *      so the baseline sits at y = tmAscent. FreeType outlines are y-up from
 *      the baseline; AppendString flips and offsets accordingly.
 *    * GDI rounds each glyph advance to an integer before accumulating, and
 *      does not apply kerning unless asked. TextExtent does the same here.
 *    * An underlined or struck-out GDI font contributes those rules to the
 *      path itself. VSFilter relies on that for \u1 and \s1, so AppendString
 *      emits them too, positioned from the face's post/OS2 tables.
 */

#pragma once

#include "../compat/platform.h"
#include "../compat/atltypes_compat.h"

#include <memory>
#include <string>
#include <vector>

namespace vsfm {

/* A GDI-format path: parallel arrays of points and PT_* type codes, exactly
 * what GetPath() produced and what Rasterizer::ScanConvert consumes. */
struct GlyphPath
{
    std::vector<POINT> points;
    std::vector<BYTE>  types;

    void clear() { points.clear(); types.clear(); }
    bool empty() const { return points.empty(); }
    size_t size() const { return points.size(); }
};

/* Mirrors the fields of LOGFONTW that actually influence rendering. */
struct FontDesc
{
    std::wstring faceName;
    int  height = 0;          // GDI semantics: >0 cell height, <0 em height
    int  weight = FW_NORMAL;
    int  charSet = DEFAULT_CHARSET;
    bool italic = false;
    bool underline = false;
    bool strikeOut = false;

    bool operator==(const FontDesc& o) const
    {
        return height == o.height && weight == o.weight && charSet == o.charSet
            && italic == o.italic && underline == o.underline
            && strikeOut == o.strikeOut && faceName == o.faceName;
    }
};

/* The subset of TEXTMETRIC the renderer reads. */
struct FontMetrics
{
    int ascent = 0;
    int descent = 0;
    int height = 0;           // ascent + descent
    int internalLeading = 0;
    int externalLeading = 0;
};

class FontFaceImpl;

/* One face at one size. Not thread safe on its own -- RTS already serialises
 * rendering behind its own lock, matching the old "one global HDC" contract. */
class FontFace
{
public:
    ~FontFace();

    const FontMetrics& Metrics() const { return m_metrics; }
    bool IsValid() const { return m_impl != nullptr; }

    /* GetTextExtentPoint32W equivalent. */
    bool TextExtent(const wchar_t* str, int len, SIZE* extent) const;

    /* Appends the outline of `str`, drawn with its cell top-left at (dx, dy),
     * to `path`, in GDI path coordinates. */
    bool AppendString(const wchar_t* str, int len, int dx, int dy, GlyphPath& path) const;

private:
    friend class FontEngine;
    FontFace();

    /* Emits an axis-aligned rectangle, used for the underline and strike-out
     * rules that a GDI path would have contained. */
    static void AppendRule(GlyphPath& path, int x, int y, int w, int h);

    std::unique_ptr<FontFaceImpl> m_impl;
    FontMetrics m_metrics;
};

class FontEngine
{
public:
    static FontEngine& Get();

    /* Returns a cached face, or nullptr if even the fallback could not be
     * opened. Callers mirror the old code: try the requested face, and on
     * failure retry with "Arial". */
    std::shared_ptr<FontFace> OpenFont(const FontDesc& desc);

    /* Registers a font supplied in memory -- e.g. an .ass [Fonts] attachment.
     * The bytes are copied. Later OpenFont calls can match `faceName`. */
    bool AddMemoryFont(const void* data, size_t len);

    void ClearMemoryFonts();

private:
    FontEngine();
    ~FontEngine();
    FontEngine(const FontEngine&) = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    /* Applies GDI's lfHeight -> rasterisation size rule and fills in metrics.
     * See the comment on the definition; this is the single most fidelity
     * critical line of the whole port. */
    static bool SetupSize(FontFaceImpl* impl, const FontDesc& desc, FontMetrics* metrics);

    static std::wstring ToLower(const std::wstring& s);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vsfm
