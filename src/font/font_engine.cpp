/*
 *  VSFilterMod cross-platform port -- FreeType glyph engine.
 *  See font_engine.h for why this emulates GDI rather than simplifying.
 */

#include "font_engine.h"
#include "font_provider.h"
#include "../compat/cstring_compat.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

#include <map>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cwctype>

namespace vsfm {

namespace {

/* GDI path type codes. PT_MOVETONC and the spline codes are VSFilter's own
 * additions for \p drawings and never appear in glyph outlines. */
const BYTE kMoveTo   = PT_MOVETO;      // 6
const BYTE kLineTo   = PT_LINETO;      // 2
const BYTE kBezierTo = PT_BEZIERTO;    // 4
const BYTE kClose    = PT_CLOSEFIGURE; // 1

inline int Round26_6(FT_Pos v) { return (int)((v + 32) >> 6); }

/* FreeType is y-up from the baseline; GDI paths are y-down from the top of the
 * cell. `baseline` is tmAscent, i.e. where the baseline sits inside the cell. */
struct PathBuilder
{
    GlyphPath* out;
    int originX;
    int originY;      // dy + tmAscent
    FT_Pos penX;      // 26.6, accumulated advance within the run
    bool contourOpen;
    FT_Vector cur;    // current point, kept in 26.6 font space

    void Emit(FT_Pos x, FT_Pos y, BYTE type)
    {
        POINT p;
        p.x = originX + Round26_6(penX + x);
        p.y = originY - Round26_6(y);
        out->points.push_back(p);
        out->types.push_back(type);
        cur.x = x;
        cur.y = y;
    }

    void CloseContour()
    {
        if (contourOpen && !out->types.empty())
            out->types.back() |= kClose;
        contourOpen = false;
    }
};

int OnMoveTo(const FT_Vector* to, void* user)
{
    PathBuilder* b = (PathBuilder*)user;
    b->CloseContour();
    b->Emit(to->x, to->y, kMoveTo);
    b->contourOpen = true;
    return 0;
}

int OnLineTo(const FT_Vector* to, void* user)
{
    PathBuilder* b = (PathBuilder*)user;
    b->Emit(to->x, to->y, kLineTo);
    return 0;
}

/* GetPath() only ever hands back cubics, and Rasterizer::ScanConvert reads
 * PT_BEZIERTO in groups of three, so TrueType quadratics are degree-elevated
 * exactly rather than approximated. */
int OnConicTo(const FT_Vector* control, const FT_Vector* to, void* user)
{
    PathBuilder* b = (PathBuilder*)user;

    const FT_Pos p0x = b->cur.x;
    const FT_Pos p0y = b->cur.y;

    const FT_Pos c1x = p0x + 2 * (control->x - p0x) / 3;
    const FT_Pos c1y = p0y + 2 * (control->y - p0y) / 3;
    const FT_Pos c2x = to->x + 2 * (control->x - to->x) / 3;
    const FT_Pos c2y = to->y + 2 * (control->y - to->y) / 3;

    b->Emit(c1x, c1y, kBezierTo);
    b->Emit(c2x, c2y, kBezierTo);
    b->Emit(to->x, to->y, kBezierTo);
    return 0;
}

int OnCubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user)
{
    PathBuilder* b = (PathBuilder*)user;
    b->Emit(c1->x, c1->y, kBezierTo);
    b->Emit(c2->x, c2->y, kBezierTo);
    b->Emit(to->x, to->y, kBezierTo);
    return 0;
}

const FT_Outline_Funcs kOutlineFuncs = {
    OnMoveTo, OnLineTo, OnConicTo, OnCubicTo, 0, 0
};

/* Decodes one code point, advancing `i`. Handles UTF-16 surrogate pairs on
 * Windows, where wchar_t is 16 bits. */
uint32_t NextCodePoint(const wchar_t* s, int len, int& i)
{
    uint32_t cp = (uint32_t)s[i++];
    if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i < len) {
        uint32_t lo = (uint32_t)s[i];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            ++i;
        }
    }
    return cp;
}

} // namespace

// ---------------------------------------------------------------------------
// Shared FT_Library
// ---------------------------------------------------------------------------

namespace {

struct Library
{
    FT_Library ft = nullptr;
    std::mutex lock;

    static Library& Get()
    {
        static Library inst;
        return inst;
    }

private:
    Library() { FT_Init_FreeType(&ft); }
    ~Library() { if (ft) FT_Done_FreeType(ft); }
};

/* A face file, opened once and shared by every size that uses it. FT_Face is
 * not size-shareable, so each FontFaceImpl gets its own FT_Face but they all
 * reference the same memory blob when the font came from an attachment. */
struct FontData
{
    std::vector<unsigned char> bytes;   // empty when loaded from a path
    std::string path;
    int faceIndex = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// FontFaceImpl
// ---------------------------------------------------------------------------

class FontFaceImpl
{
public:
    FT_Face face = nullptr;
    std::shared_ptr<FontData> data;

    bool syntheticBold = false;
    bool syntheticItalic = false;
    bool underline = false;
    bool strikeOut = false;

    /* Non-identity when orient is in play; rotates each glyph about its own
     * origin without tilting the baseline, as GM_ADVANCED + lfOrientation did. */
    bool hasRotation = false;
    FT_Matrix rotation = { 0x10000, 0, 0, 0x10000 };

    /* In GDI path units (= pixels at the requested cell height). */
    int underlinePos = 0;       // distance below the baseline to the top edge
    int underlineThickness = 1;
    int strikeOutPos = 0;
    int strikeOutThickness = 1;

    int emSize = 0;

    ~FontFaceImpl()
    {
        if (face) {
            std::lock_guard<std::mutex> g(Library::Get().lock);
            FT_Done_Face(face);
        }
    }

    FT_UInt GlyphIndex(uint32_t cp) const
    {
        FT_UInt gi = FT_Get_Char_Index(face, cp);
        if (gi) return gi;

        /* Symbol fonts (Wingdings and friends) map their glyphs into the
         * private-use block at 0xF000. GDI does this remap implicitly. */
        if (cp < 0x100) {
            gi = FT_Get_Char_Index(face, 0xF000 + cp);
            if (gi) return gi;
        }
        return 0;
    }

    bool LoadGlyph(FT_UInt gi) const
    {
        /* NO_HINTING keeps the outline identical on every platform; at the 8x
         * supersampled sizes RTS asks for, hinting would only add noise. */
        FT_Int32 flags = FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING;
        if (FT_Load_Glyph(face, gi, flags) != 0) return false;
        if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;

        if (syntheticItalic) FT_GlyphSlot_Oblique(face->glyph);
        if (syntheticBold)   FT_GlyphSlot_Embolden(face->glyph);
        if (hasRotation)     FT_Outline_Transform(&face->glyph->outline, &rotation);
        return true;
    }
};

// ---------------------------------------------------------------------------
// FontFace
// ---------------------------------------------------------------------------

FontFace::FontFace() {}
FontFace::~FontFace() {}

bool FontFace::TextExtent(const wchar_t* str, int len, SIZE* extent) const
{
    if (!extent) return false;
    extent->cx = 0;
    extent->cy = m_metrics.height;

    if (!m_impl || !str) return false;
    if (len < 0) len = (int)std::wcslen(str);

    std::lock_guard<std::mutex> g(Library::Get().lock);

    int total = 0;
    int i = 0;
    while (i < len) {
        uint32_t cp = NextCodePoint(str, len, i);
        FT_UInt gi = m_impl->GlyphIndex(cp);
        if (!m_impl->LoadGlyph(gi)) continue;
        /* GDI rounds each advance before accumulating; matching that keeps
         * per-character and whole-run measurements consistent, which the
         * \fsp path in CText depends on. */
        total += Round26_6(m_impl->face->glyph->advance.x);
    }

    extent->cx = total;
    return true;
}

bool FontFace::AppendString(const wchar_t* str, int len, int dx, int dy, GlyphPath& path) const
{
    if (!m_impl || !str) return false;
    if (len < 0) len = (int)std::wcslen(str);

    std::lock_guard<std::mutex> g(Library::Get().lock);

    PathBuilder b;
    b.out = &path;
    b.originX = dx;
    b.originY = dy + m_metrics.ascent;
    b.penX = 0;
    b.contourOpen = false;
    b.cur.x = b.cur.y = 0;

    int runStart = (int)path.points.size();

    int i = 0;
    while (i < len) {
        uint32_t cp = NextCodePoint(str, len, i);
        FT_UInt gi = m_impl->GlyphIndex(cp);
        if (!m_impl->LoadGlyph(gi)) continue;

        FT_Outline_Decompose(&m_impl->face->glyph->outline, &kOutlineFuncs, &b);
        b.CloseContour();

        b.penX += (FT_Pos)Round26_6(m_impl->face->glyph->advance.x) << 6;
    }

    const int runWidth = Round26_6(b.penX);

    /* GDI folds the underline and strike-out rules into the path itself. */
    if (m_impl->underline && runWidth > 0)
        AppendRule(path, dx, b.originY + m_impl->underlinePos,
                   runWidth, m_impl->underlineThickness);

    if (m_impl->strikeOut && runWidth > 0)
        AppendRule(path, dx, b.originY - m_impl->strikeOutPos,
                   runWidth, m_impl->strikeOutThickness);

    return (int)path.points.size() > runStart || runWidth == 0;
}

void FontFace::AppendRule(GlyphPath& path, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;

    const POINT quad[4] = {
        { x,     y     },
        { x + w, y     },
        { x + w, y + h },
        { x,     y + h },
    };

    path.points.push_back(quad[0]); path.types.push_back(kMoveTo);
    path.points.push_back(quad[1]); path.types.push_back(kLineTo);
    path.points.push_back(quad[2]); path.types.push_back(kLineTo);
    path.points.push_back(quad[3]); path.types.push_back(kLineTo | kClose);
}

// ---------------------------------------------------------------------------
// FontEngine
// ---------------------------------------------------------------------------

struct FontEngine::Impl
{
    std::mutex lock;

    /* Face cache. The key is the full descriptor: different sizes need
     * different FT_Faces because FT_Set_Pixel_Sizes is per-face state. */
    struct Key
    {
        std::wstring face;
        int height, weight, charSet, orientation;
        bool italic, underline, strikeOut;

        bool operator<(const Key& o) const
        {
            if (orientation != o.orientation) return orientation < o.orientation;
            if (height != o.height) return height < o.height;
            if (weight != o.weight) return weight < o.weight;
            if (charSet != o.charSet) return charSet < o.charSet;
            if (italic != o.italic) return italic < o.italic;
            if (underline != o.underline) return underline < o.underline;
            if (strikeOut != o.strikeOut) return strikeOut < o.strikeOut;
            return face < o.face;
        }
    };

    std::map<Key, std::weak_ptr<FontFace> > cache;

    /* Fonts supplied by the script itself (an .ass [Fonts] attachment), keyed
     * by lower-cased family name. Looked up before the system provider so an
     * attachment wins over a same-named installed font, which is what an
     * author embedding a font is asking for. */
    struct MemoryFont
    {
        std::shared_ptr<FontData> data;
        int faceIndex = 0;
    };
    std::map<std::wstring, MemoryFont> memoryFonts;

    /* Font files are read into memory once and shared by every size that uses
     * them. Besides saving re-reads, this sidesteps FT_New_Face's use of
     * fopen(), which on Windows cannot open a path outside the active ANSI
     * code page -- a real failure mode for user-installed fonts. */
    std::map<std::string, std::shared_ptr<FontData> > blobs;

    std::shared_ptr<FontData> LoadBlob(const std::string& path, int faceIndex);

    static Key MakeKey(const FontDesc& d)
    {
        Key k;
        k.face = d.faceName;
        k.height = d.height;
        k.weight = d.weight;
        k.charSet = d.charSet;
        k.orientation = d.orientation;
        k.italic = d.italic;
        k.underline = d.underline;
        k.strikeOut = d.strikeOut;
        return k;
    }
};

std::shared_ptr<FontData> FontEngine::Impl::LoadBlob(const std::string& path, int /*faceIndex*/)
{
    auto it = blobs.find(path);
    if (it != blobs.end()) return it->second;

    FILE* fp = nullptr;
#if defined(_WIN32)
    /* The path came back from DirectWrite as UTF-8; widen it so fonts under a
     * non-ANSI path still open. */
    const std::wstring wide = vsfm::U82W(path.c_str(), path.size());
    fp = _wfopen(wide.c_str(), L"rb");
#else
    fp = std::fopen(path.c_str(), "rb");
#endif
    if (!fp) return nullptr;

    auto fd = std::make_shared<FontData>();
    fd->path = path;

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    if (size > 0) {
        fd->bytes.resize((size_t)size);
        if (std::fread(fd->bytes.data(), 1, (size_t)size, fp) != (size_t)size)
            fd->bytes.clear();
    }
    std::fclose(fp);

    if (fd->bytes.empty()) return nullptr;

    blobs[path] = fd;
    return fd;
}

FontEngine::FontEngine() : m_impl(new Impl) {}
FontEngine::~FontEngine() {}

FontEngine& FontEngine::Get()
{
    static FontEngine inst;
    return inst;
}

bool FontEngine::AddMemoryFont(const void* data, size_t len)
{
    if (!data || len == 0) return false;

    auto fd = std::make_shared<FontData>();
    fd->bytes.assign((const unsigned char*)data, (const unsigned char*)data + len);

    /* Register every face in the blob: a .ttc attachment carries several. */
    bool added = false;
    long faceCount = 1;

    for (long i = 0; i < faceCount; ++i) {
        FT_Face probe = nullptr;
        {
            std::lock_guard<std::mutex> g(Library::Get().lock);
            if (FT_New_Memory_Face(Library::Get().ft, fd->bytes.data(),
                                   (FT_Long)fd->bytes.size(), i, &probe) != 0)
                break;
        }

        if (i == 0) faceCount = probe->num_faces > 0 ? probe->num_faces : 1;

        std::wstring family;
        if (probe->family_name)
            for (const char* p = probe->family_name; *p; ++p)
                family += (wchar_t)(unsigned char)*p;

        {
            std::lock_guard<std::mutex> g(Library::Get().lock);
            FT_Done_Face(probe);
        }

        if (family.empty()) continue;

        std::lock_guard<std::mutex> g(m_impl->lock);
        Impl::MemoryFont mf;
        mf.data = fd;
        mf.faceIndex = (int)i;
        m_impl->memoryFonts[ToLower(family)] = mf;
        FontProvider::Get().AddMemoryFamily(family);
        added = true;
    }

    return added;
}

std::wstring FontEngine::ToLower(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) out += (wchar_t)std::towlower((wint_t)c);
    return out;
}

void FontEngine::ClearMemoryFonts()
{
    std::lock_guard<std::mutex> g(m_impl->lock);
    m_impl->memoryFonts.clear();
}

std::shared_ptr<FontFace> FontEngine::OpenFont(const FontDesc& desc)
{
    std::lock_guard<std::mutex> g(m_impl->lock);

    const Impl::Key key = Impl::MakeKey(desc);
    auto it = m_impl->cache.find(key);
    if (it != m_impl->cache.end()) {
        if (auto live = it->second.lock()) return live;
        m_impl->cache.erase(it);
    }

    FontMatch match;
    std::shared_ptr<FontData> blob;
    bool fromMemory = false;

    /* Script-embedded fonts take priority over anything installed. */
    auto mem = m_impl->memoryFonts.find(ToLower(desc.faceName));
    if (mem != m_impl->memoryFonts.end()) {
        blob = mem->second.data;
        match.faceIndex = mem->second.faceIndex;
        match.exactFamily = true;
        /* Whether bold/italic must be faked depends on what the attachment
         * already is; decided below, once the face is open. */
        fromMemory = true;
    } else {
        if (!FontProvider::Get().Match(desc.faceName, desc.weight, desc.italic,
                                       desc.charSet, &match))
            return nullptr;
        blob = m_impl->LoadBlob(match.path, match.faceIndex);
    }

    if (!blob) return nullptr;

    std::shared_ptr<FontFace> face(new FontFace());
    std::unique_ptr<FontFaceImpl> impl(new FontFaceImpl());

    {
        std::lock_guard<std::mutex> lg(Library::Get().lock);
        if (FT_New_Memory_Face(Library::Get().ft, blob->bytes.data(),
                               (FT_Long)blob->bytes.size(),
                               match.faceIndex, &impl->face) != 0)
            return nullptr;

        if (FT_Select_Charmap(impl->face, FT_ENCODING_UNICODE) != 0)
            FT_Select_Charmap(impl->face, FT_ENCODING_MS_SYMBOL);
    }

    if (fromMemory) {
        const bool isBold = (impl->face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
        const bool isItalic = (impl->face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
        match.syntheticBold = (desc.weight >= FW_BOLD) && !isBold;
        match.syntheticItalic = desc.italic && !isItalic;
    }

    if (desc.orientation % 3600 != 0) {
        const double rad = (double)desc.orientation * 3.14159265358979323846 / 1800.0;
        const FT_Fixed c = (FT_Fixed)(cos(rad) * 65536.0 + 0.5);
        const FT_Fixed sn = (FT_Fixed)(sin(rad) * 65536.0 + 0.5);
        /* FreeType is y-up, so a positive lfOrientation (counter-clockwise on
         * screen) is a positive rotation here too. */
        impl->rotation.xx = c;  impl->rotation.xy = -sn;
        impl->rotation.yx = sn; impl->rotation.yy = c;
        impl->hasRotation = true;
    }

    impl->syntheticBold = match.syntheticBold;
    impl->syntheticItalic = match.syntheticItalic;
    impl->underline = desc.underline;
    impl->strikeOut = desc.strikeOut;
    impl->data = blob;   // keeps the bytes alive for the FT_Face's lifetime

    if (!SetupSize(impl.get(), desc, &face->m_metrics))
        return nullptr;

    face->m_impl = std::move(impl);
    m_impl->cache[key] = face;
    return face;
}

/* Reproduces GDI's mapping from LOGFONT::lfHeight to a rasterisation size.
 *
 * With lfHeight > 0 GDI treats the value as the character *cell* height and
 * picks the em size such that tmAscent + tmDescent == lfHeight. For a TrueType
 * font those come from OS/2's usWinAscent / usWinDescent, so:
 *
 *     ppem = lfHeight * unitsPerEm / (usWinAscent + usWinDescent)
 *
 * With lfHeight < 0 the magnitude *is* the em size and no scaling is needed.
 * Getting this backwards is the classic reason a VSFilter script rendered
 * through libass comes out with the wrong font size.
 */
bool FontEngine::SetupSize(FontFaceImpl* impl, const FontDesc& desc, FontMetrics* metrics)
{
    FT_Face f = impl->face;
    if (!f) return false;

    std::lock_guard<std::mutex> g(Library::Get().lock);

    const int requested = desc.height;
    if (requested == 0) return false;

    int winAscent, winDescent;
    const TT_OS2* os2 = (const TT_OS2*)FT_Get_Sfnt_Table(f, FT_SFNT_OS2);

    if (os2 && os2->version != 0xFFFF && (os2->usWinAscent + os2->usWinDescent) > 0) {
        winAscent = os2->usWinAscent;
        winDescent = os2->usWinDescent;
    } else if (FT_IS_SCALABLE(f)) {
        winAscent = f->ascender;
        winDescent = -f->descender;
    } else {
        winAscent = f->units_per_EM;
        winDescent = 0;
    }

    int emPixels;
    if (requested > 0) {
        const int cell = winAscent + winDescent;
        if (cell <= 0) return false;
        emPixels = MulDiv(requested, f->units_per_EM, cell);
    } else {
        emPixels = -requested;
    }
    if (emPixels < 1) emPixels = 1;

    if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)emPixels) != 0) return false;
    impl->emSize = emPixels;

    const FT_Fixed yScale = f->size->metrics.y_scale;

    metrics->ascent  = Round26_6(FT_MulFix(winAscent, yScale));
    metrics->descent = Round26_6(FT_MulFix(winDescent, yScale));
    metrics->height  = metrics->ascent + metrics->descent;
    metrics->internalLeading = metrics->height - emPixels;
    if (metrics->internalLeading < 0) metrics->internalLeading = 0;
    metrics->externalLeading = 0;

    /* post-table rule geometry, in the same y-down path units. */
    int upos = f->underline_position;
    int uthick = f->underline_thickness;
    if (uthick <= 0) uthick = f->units_per_EM / 14;

    impl->underlinePos = Round26_6(FT_MulFix(-upos, yScale));
    impl->underlineThickness = Round26_6(FT_MulFix(uthick, yScale));
    if (impl->underlineThickness < 1) impl->underlineThickness = 1;

    if (os2 && os2->version != 0xFFFF && os2->yStrikeoutSize > 0) {
        impl->strikeOutPos = Round26_6(FT_MulFix(os2->yStrikeoutPosition, yScale));
        impl->strikeOutThickness = Round26_6(FT_MulFix(os2->yStrikeoutSize, yScale));
    } else {
        impl->strikeOutPos = Round26_6(FT_MulFix(f->units_per_EM / 4, yScale));
        impl->strikeOutThickness = impl->underlineThickness;
    }
    if (impl->strikeOutThickness < 1) impl->strikeOutThickness = 1;

    return true;
}

} // namespace vsfm
