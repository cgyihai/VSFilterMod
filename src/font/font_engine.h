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

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
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

    /* LOGFONT::lfOrientation, in tenths of a degree counter-clockwise. Drives
     * VSFilterMod's \frs tag (patch m007), which on Windows put the HDC in
     * GM_ADVANCED so GDI rotated each glyph while keeping the run horizontal.
     * GDI also changes the font's metrics, spacing and extents when it does
     * that; font_engine.cpp reproduces all of it. */
    int orientation = 0;

    bool operator==(const FontDesc& o) const
    {
        return height == o.height && weight == o.weight && charSet == o.charSet
            && italic == o.italic && underline == o.underline
            && strikeOut == o.strikeOut && orientation == o.orientation
            && faceName == o.faceName;
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
class FontFace;
struct FontData;
struct FontMatch;

/* Where a code point's glyph comes from: the face itself, or the fallback
 * face that stands in for it (see "Glyph fallback" in font_engine.cpp). */
struct GlyphSource
{
    const FontFaceImpl* impl = nullptr;
    unsigned glyph = 0;
    uint32_t codePoint = 0;
};

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

    /* Finds a fallback face for every code point of `str` this face has no
     * glyph for. Opens fonts, so it must run before the FreeType lock is
     * taken; Resolve then only reads what it found. */
    void PrepareFallbacks(const wchar_t* str, int len) const;
    GlyphSource Resolve(uint32_t cp) const;
    /* Whether any character of `str` is drawn from a fallback face. */
    bool RunUsesFallback(const wchar_t* str, int len) const;

    /* Emits an axis-aligned rectangle, used for the underline and strike-out
     * rules that a GDI path would have contained. */
    static void AppendRule(GlyphPath& path, int x, int y, int w, int h);

    std::unique_ptr<FontFaceImpl> m_impl;
    FontMetrics m_metrics;
    FontDesc m_desc;

    struct Fallback
    {
        std::shared_ptr<FontFace> face;   // null: nothing installed covers it
        unsigned glyph = 0;
    };
    mutable std::mutex m_fallbackLock;
    mutable std::map<uint32_t, Fallback> m_fallbacks;
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
    friend class FontFace;

    /* The face that draws `cp` when `base` has no glyph for it, and that
     * glyph's index; a null face when no installed font covers it. */
    std::shared_ptr<FontFace> FindFallback(const FontDesc& base, int baseEm,
                                           uint32_t cp, unsigned* glyph);

    /* OpenFont, but only when the family really is installed or embedded:
     * a fallback that the platform quietly substituted would be no fallback. */
    std::shared_ptr<FontFace> OpenExactFont(const FontDesc& desc);
    std::shared_ptr<FontFace> OpenFontImpl(const FontDesc& desc, bool exactOnly);

    /* Opens `blob` as the face `match` names, sized and styled per `desc`.
     * Caller holds m_impl->lock. */
    std::shared_ptr<FontFace> Build(const FontDesc& desc, FontMatch match,
                                    const std::shared_ptr<FontData>& blob,
                                    bool fromMemory, bool vertical);

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
