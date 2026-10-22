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
#include FT_BBOX_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>
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

    /* A rotated font's glyph turns about (pivotX - originX, 0) on screen, the
     * point (halfWidth, ascent) of the glyph measured from its origin, y up.
     * See "GDI's layout of a rotated font". */
    bool rotate = false;
    double pivotX = 0.0;
    double halfWidth = 0.0;
    double ascent = 0.0;
    double cosA = 1.0, sinA = 0.0;

    /* An '@' font's CJK glyph lies on its side, turned a quarter
     * counter-clockwise: the point `vertOriginY` above the baseline goes to
     * the pen, and the glyph's x = 0 goes `vertBottom` below the top of the
     * cell. Both 26.6. See "Vertical ('@') fonts". */
    bool sideways = false;
    FT_Pos vertOriginY = 0;
    FT_Pos vertBottom = 0;
    int cellTop = 0;

    void Emit(FT_Pos x, FT_Pos y, BYTE type)
    {
        POINT p;
        if (sideways) {
            p.x = originX + Round26_6(penX + vertOriginY - y);
            p.y = cellTop + Round26_6(vertBottom - x);
        } else if (rotate) {
            const double qx = x / 64.0 - halfWidth;
            const double qy = y / 64.0 - ascent;
            p.x = (int)std::lround(pivotX + qx * cosA - qy * sinA);
            p.y = (int)std::lround(originY - (qx * sinA + qy * cosA));
        } else {
            p.x = originX + Round26_6(penX + x);
            p.y = originY - Round26_6(y);
        }
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

} // namespace

/* A face file, opened once and shared by every size that uses it. FT_Face is
 * not size-shareable, so each FontFaceImpl gets its own FT_Face but they all
 * reference the same memory blob when the font came from an attachment. */
struct FontData
{
    std::vector<unsigned char> bytes;   // empty when loaded from a path
    std::string path;
    int faceIndex = 0;
};

// ---------------------------------------------------------------------------
// GDI's simulated bold
// ---------------------------------------------------------------------------
//
// Worked out by comparing GetGlyphOutline(GGO_NATIVE | GGO_UNHINTED) of the
// same glyph at weight 400 and 700, point by point, on SimHei, SimSun, KaiTi
// and Impact from 40 to 8000 ppem, and on a synthetic font of polygons. It
// is not an outward offset of the outline:
//
//  * With Sy = ceil(ppem / 50) - 1 and Sx = Sy + 1 (whole pixels), the glyph
//    is first scaled by (ppem - Sy) / ppem about its origin -- the baseline
//    point -- so it comes out about 2% smaller.
//
//  * Then every point moves by (Sx/2 * (1 + vx), Sy/2 * (1 + vy)), v being
//    the point's miter: the sum of the outward unit normals of its two edges
//    divided by 1 + their dot product -- a straight edge moves by S/2 along
//    its normal. So the left and bottom edges stay put and the right and top
//    ones move by the full S, as with FT_Outline_EmboldenXY, but GDI clamps
//    each component of v to [-1, 1] where FreeType limits the miter by the
//    edge lengths instead, which matters at the sharp serifs CJK strokes end
//    in. Whether a point is on or off the curve makes no difference.
//
//  * The advance grows by one pixel, regardless of size; an '@' face's
//    vertical advance scales with the glyph and then grows by one pixel.
//
//  * A rotated font (\frs) is emboldened in glyph space too, then turned,
//    but both strengths are divided by max(|cos a|, |sin a|) -- 15% heavier
//    at 30 degrees, 41% at 45. The shrink does not change.
//
// A stroke therefore thickens by S - 2% of its width, and a counter shrinks
// by the same. The residual against GDI is under ppem/1000 per point on
// polygons; a few points next to a clamped corner or a very short edge land
// up to ppem/150 off, for a reason not found.

namespace {

struct GdiBold
{
    double sx, sy;      // strengths, pixels
    FT_Fixed scale;     // (ppem - Sy) / ppem
};

GdiBold GdiBoldFor(int ppem, double rotCos, double rotSin)
{
    GdiBold b;
    if (ppem < 1) ppem = 1;
    const int sy = (ppem + 49) / 50 - 1;
    b.scale = FT_DivFix(ppem - sy, ppem);
    const double stretch = 1.0 / std::max(std::fabs(rotCos), std::fabs(rotSin));
    b.sy = sy * stretch;
    b.sx = (sy + 1) * stretch;
    return b;
}

void EmboldenLikeGdi(FT_Outline* outline, const GdiBold& bold)
{
    const FT_Matrix shrink = { bold.scale, 0, 0, bold.scale };
    FT_Outline_Transform(outline, &shrink);

    const FT_Orientation orientation = FT_Outline_Get_Orientation(outline);
    if (orientation == FT_ORIENTATION_NONE) return;
    /* Outward is to the left of travel for a clockwise (TrueType) outline
     * in FreeType's y-up space. */
    const double side = (orientation == FT_ORIENTATION_TRUETYPE) ? 1.0 : -1.0;

    FT_Vector* const pts = outline->points;
    std::vector<FT_Vector> moved(pts, pts + outline->n_points);

    int first = 0;
    for (int c = 0; c < outline->n_contours; ++c) {
        const int last = outline->contours[c];
        const int n = last - first + 1;

        for (int j = first; j <= last; ++j) {
            /* The nearest points that do not coincide with this one. */
            int prev = j, next = j;
            for (int k = 1; k < n; ++k) {
                const int i = first + (j - first - k + n) % n;
                if (pts[i].x != pts[j].x || pts[i].y != pts[j].y) { prev = i; break; }
            }
            for (int k = 1; k < n; ++k) {
                const int i = first + (j - first + k) % n;
                if (pts[i].x != pts[j].x || pts[i].y != pts[j].y) { next = i; break; }
            }
            if (prev == j || next == j) continue;

            double ix = (double)(pts[j].x - pts[prev].x), iy = (double)(pts[j].y - pts[prev].y);
            double ox = (double)(pts[next].x - pts[j].x), oy = (double)(pts[next].y - pts[j].y);
            const double il = std::sqrt(ix * ix + iy * iy), ol = std::sqrt(ox * ox + oy * oy);
            ix /= il; iy /= il; ox /= ol; oy /= ol;

            const double n1x = -iy * side, n1y = ix * side;
            const double n2x = -oy * side, n2y = ox * side;
            const double d = 1.0 + n1x * n2x + n1y * n2y;

            double vx, vy;
            if (d > 1.0 / 1024) {
                vx = (n1x + n2x) / d;
                vy = (n1y + n2y) / d;
            } else {
                /* The outline doubles back on itself: the tip moves on. */
                vx = ix * 1024;
                vy = iy * 1024;
            }
            vx = std::min(1.0, std::max(-1.0, vx));
            vy = std::min(1.0, std::max(-1.0, vy));

            moved[j].x += (FT_Pos)std::lround(bold.sx * 32 * (1.0 + vx));
            moved[j].y += (FT_Pos)std::lround(bold.sy * 32 * (1.0 + vy));
        }
        first = last + 1;
    }
    std::copy(moved.begin(), moved.end(), pts);
}

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
    /* False when the platform substituted another family for the one asked
     * for; such a face is never used as a glyph fallback. */
    bool exactFamily = true;
    bool underline = false;
    bool strikeOut = false;

    /* lfOrientation != 0 under GM_ADVANCED (\frs): GDI rotates every glyph
     * but keeps the run horizontal, spacing and placing the glyphs by rules
     * of its own -- see "GDI's layout of a rotated font" below. rotCos and
     * rotSin are of the counter-clockwise orientation angle. */
    bool rotated = false;
    double rotCos = 1.0;
    double rotSin = 0.0;

    /* An '@' face: CJK glyphs lie on their side, in their GSUB 'vert' forms.
     * See "Vertical ('@') fonts" below. */
    bool vertical = false;
    std::map<FT_UInt, FT_UInt> vertForms;

    /* The GPOS table and its 'kern' PairPos subtables (offsets into it),
     * read the first time a fallback run needs kerning. Guarded by the
     * FreeType lock, which every caller holds. */
    mutable bool gposRead = false;
    mutable std::vector<unsigned char> gpos;
    mutable std::vector<size_t> gposKern;
    mutable bool gposHasKern = false;   // a 'kern' feature, whatever its lookups
    FT_Pos vertBottom = 0;      // 26.6: usWinAscent + |sTypoDescender|
    mutable FT_Pos vertOriginY = 0;   // 26.6, of the glyph LoadGlyph last loaded

    /* Whether LoadGlyph last emboldened its glyph, and that glyph's exact
     * bounding box before it did (26.6). A rotated run measures a simulated
     * bold glyph from these; see "GDI's layout of a rotated font". */
    mutable bool lastEmboldened = false;
    mutable FT_BBox lastPlainBox = { 0, 0, 0, 0 };

    /* In GDI path units (= pixels at the requested cell height). */
    int underlinePos = 0;       // distance below the baseline to the top edge
    int underlineThickness = 1;
    int strikeOutPos = 0;
    int strikeOutThickness = 1;

    int emSize = 0;
    bool cellIsEm = false;      // OS/2 usWinAscent + usWinDescent == unitsPerEm

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

        if (vertical) {
            FT_BBox cbox;
            FT_Outline_Get_CBox(&face->glyph->outline, &cbox);
            vertOriginY = cbox.yMax + face->glyph->metrics.vertBearingY;
        }

        /* See "GDI's simulated bold" above. FT_GlyphSlot_Embolden is the
         * wrong tool: em/24, not shrunk first, and the advance grows by the
         * same amount. */
        lastEmboldened = syntheticBold && face->glyph->outline.n_points > 0;
        if (lastEmboldened)
            FT_Outline_Get_BBox(&face->glyph->outline, &lastPlainBox);
        if (syntheticBold) {
            const GdiBold bold = GdiBoldFor(emSize, rotCos, rotSin);
            EmboldenLikeGdi(&face->glyph->outline, bold);
            face->glyph->advance.x += 64;
            face->glyph->metrics.vertAdvance =
                ((FT_Pos)Round26_6(FT_MulFix(face->glyph->metrics.vertAdvance, bold.scale)) + 1) << 6;
        }

        /* GDI's simulated italic is a shear of 0.3397 (about 18.8 degrees),
         * measured against upstream VSFilterMod as constant across SimHei,
         * SimSun and Microsoft YaHei at 120 and 300px. FT_GlyphSlot_Oblique
         * uses tan(12 deg) = 0.2126, which leans visibly less. Sheared about
         * the baseline, so the advance and the baseline point are unchanged.
         * GDI shears a simulated bold glyph after emboldening it. */
        if (syntheticItalic) {
            static const FT_Matrix kGdiOblique = { 0x10000, 22263, 0, 0x10000 };
            FT_Outline_Transform(&face->glyph->outline, &kGdiOblique);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// GDI's layout of a rotated font (lfOrientation != lfEscapement, GM_ADVANCED)
// ---------------------------------------------------------------------------
//
// Undocumented, and nothing like "rotate each glyph about its origin". Worked
// out by probing GDI directly -- TextOutW into a path, per glyph, against the
// unrotated outline -- on system fonts and on a synthetic font whose glyphs
// are rectangles of chosen size and place (tests/ has none of that; the
// numbers below are what those probes settled on):
//
//  * The rotated font reports its own TEXTMETRIC. Away from the axes its
//    ascent and descent come from the head table's bounding box, on the axes
//    (90 and 270 degrees) from usWinAscent/usWinDescent, and in both cases
//    grow by unitsPerEm/64 and round up. At 0 and 180 degrees they are the
//    unrotated values. RTS reads them (CMyFont::m_ascent), so they move the
//    line as well as the glyphs.
//
//  * Each glyph turns about the top centre of its rotated cell: (w/2, ascent)
//    from its origin, w being the advance. That point lands on the TA_TOP
//    line, y = 0.
//
//  * The pen advance is two half-chords of ellipses through the baseline
//    centre, one for the part of the glyph above the baseline and one for the
//    part below:
//
//        r(A, B) = 1 / sqrt((cos a / A)^2 + (sin a / B)^2)
//        up   = r(w/2, H/2 + max(0, ink top))
//        down = r(w/2, H/2 - min(0, ink bottom))       H = ascent / 8
//
//    advance = up + down. Where the ink sits horizontally does not matter.
//    The pivot sits at pen + whichever half faces left on screen: `up` while
//    the glyph leans left (0 < a < 180), `down` otherwise.
//
//  * GetTextExtentPoint32 measures along the rotated axes: cx is the span of
//    the glyphs' ink boxes along the rotated baseline, plus one (an empty
//    glyph counts as a one-pixel box at its origin); cy is the cell height
//    plus the pivots' spread along the rotated ascender. RTS feeds both into
//    CText::m_width, so they decide where the line is centred.
//
//  * All of that spacing and measuring is dropped for a run that contains a
//    CJK character (see RunKeepsAdvances): every glyph of such a run steps by
//    its plain advance, and the extent is the plain one -- the advances
//    summed, by the rotated cell height. Rotation and pivots are as above.
//
//  * A glyph taken from a fallback face (see "Glyph fallback") always steps
//    by the ellipse rule, even in such a run, and so does a space after one
//    (see RunSegments); the run's own glyphs still follow the rule above,
//    kerned. A run that needs fallback glyphs is measured by the ellipse
//    rule's cx, with cy left at the cell height.
//
// Checked against GDI to within a device pixel -- an eighth of an output
// pixel at the 8x size RTS renders at -- for Latin text in Arial, Times New
// Roman and synthetic test fonts, and for CJK runs in SimSun and Microsoft
// YaHei, at angles all round the circle. The metrics rule was also checked on
// Segoe UI, Verdana, Courier New and Georgia.

namespace {

/* Whether GDI lays a run out with plain advances even though the font is
 * rotated: it does as soon as the run holds one of these characters. They are
 * near enough the characters that stand upright in vertical CJK text. GDI's
 * own test could not be recovered -- it is not the font's code pages, GSUB
 * 'vert' coverage, vmtx or embedded bitmaps -- so this is the set SimSun,
 * SimHei, Microsoft YaHei, MS Gothic, Yu Gothic, Malgun Gothic and Arial were
 * measured to agree on. Kana are in it, as for YaHei and the Japanese fonts;
 * SimSun and SimHei alone leave a run of kana to the ellipse rule. */
bool KeepsAdvanceWhenRotated(uint32_t cp)
{
    static const uint32_t kRanges[][2] = {
        { 0x1100, 0x11FF },  // Hangul Jamo
        { 0x2015, 0x2015 }, { 0x203B, 0x203B }, { 0x2121, 0x2121 },
        { 0x2190, 0x2193 },  // arrows
        { 0x221E, 0x221E }, { 0x2225, 0x2225 }, { 0x2227, 0x222A },
        { 0x2234, 0x2235 }, { 0x2261, 0x2261 }, { 0x22A5, 0x22A5 },
        { 0x2312, 0x2312 },
        { 0x2460, 0x24FF },  // enclosed alphanumerics
        /* The box-drawing characters GB2312 and JIS X 0208 carry. */
        { 0x2500, 0x2503 }, { 0x250C, 0x250C }, { 0x250F, 0x2510 },
        { 0x2513, 0x2514 }, { 0x2517, 0x2518 }, { 0x251B, 0x251D },
        { 0x2520, 0x2520 }, { 0x2523, 0x2525 }, { 0x2528, 0x2528 },
        { 0x252B, 0x252C }, { 0x252F, 0x2530 }, { 0x2533, 0x2534 },
        { 0x2537, 0x2538 }, { 0x253B, 0x253C }, { 0x253F, 0x253F },
        { 0x2542, 0x2542 }, { 0x254B, 0x254B },
        { 0x25A0, 0x25A1 }, { 0x25B2, 0x25B3 }, { 0x25BC, 0x25BD },
        { 0x25C6, 0x25C7 }, { 0x25CB, 0x25CB }, { 0x25CE, 0x25CF },
        { 0x2605, 0x2606 }, { 0x2640, 0x2640 }, { 0x2642, 0x2642 },
        { 0x2E80, 0x2FFF },  // CJK radicals, Kangxi, description characters
        { 0x3001, 0x303F },  // CJK symbols and punctuation
        { 0x3040, 0x30FF },  // kana
        { 0x3130, 0x318F },  // Hangul compatibility jamo
        { 0x3190, 0x31FF },  // kanbun, strokes, katakana extension
        { 0x3200, 0x33FF },  // enclosed CJK, CJK compatibility
        { 0x3400, 0x4DBF },  // CJK extension A
        { 0x4E00, 0x9FFF },  // CJK unified ideographs
        { 0xAC00, 0xD7AF },  // Hangul syllables
        { 0xF900, 0xFAFF },  // CJK compatibility ideographs
        { 0xFE10, 0xFE19 },  // vertical forms
        { 0xFE30, 0xFE6F },  // CJK compatibility and small forms
        { 0xFF01, 0xFF9F },  // fullwidth ASCII, halfwidth katakana
        { 0xFFE0, 0xFFEE },
        { 0x20000, 0x3FFFF }, // CJK extensions B and later
    };
    for (const auto& r : kRanges)
        if (cp >= r[0] && cp <= r[1]) return true;
    return false;
}

bool RunKeepsAdvances(const wchar_t* str, int len)
{
    int i = 0;
    while (i < len)
        if (KeepsAdvanceWhenRotated(NextCodePoint(str, len, i))) return true;
    return false;
}

/* Reads the face's GPOS table and collects the PairPos subtables of its
 * 'kern' feature lookups, any script, unwrapping extension lookups. */
void ReadGposKern(const FontFaceImpl* impl)
{
    impl->gposRead = true;
    const FT_ULong tag = FT_MAKE_TAG('G', 'P', 'O', 'S');
    FT_ULong len = 0;
    if (FT_Load_Sfnt_Table(impl->face, tag, 0, nullptr, &len) != 0 || len < 10) return;
    std::vector<unsigned char>& t = impl->gpos;
    t.resize(len);
    if (FT_Load_Sfnt_Table(impl->face, tag, 0, t.data(), &len) != 0) { t.clear(); return; }

    auto u16 = [&](size_t o) -> unsigned {
        return o + 2 <= t.size() ? (unsigned)((t[o] << 8) | t[o + 1]) : 0u;
    };
    const size_t featureList = u16(6), lookupList = u16(8);
    std::set<unsigned> lookups;
    const unsigned features = u16(featureList);
    for (unsigned i = 0; i < features; ++i) {
        const size_t rec = featureList + 2 + 6 * i;
        if (rec + 6 > t.size() || std::memcmp(&t[rec], "kern", 4) != 0) continue;
        const size_t feature = featureList + u16(rec + 4);
        const unsigned n = u16(feature + 2);
        for (unsigned k = 0; k < n; ++k) lookups.insert(u16(feature + 4 + 2 * k));
    }
    impl->gposHasKern = !lookups.empty();
    /* Only plain pair adjustments are applied. Contextual kerning (lookup
     * types 7 and 8, as in Tahoma) is left out: it kerns nothing outside its
     * contexts, and the probes found nothing it kerned. */
    for (unsigned li : lookups) {
        if (li >= u16(lookupList)) continue;
        const size_t lookup = lookupList + u16(lookupList + 2 + 2 * li);
        const unsigned type = u16(lookup), subtables = u16(lookup + 4);
        for (unsigned k = 0; k < subtables; ++k) {
            size_t sub = lookup + u16(lookup + 6 + 2 * k);
            if (type == 9) {
                if (u16(sub) != 1 || u16(sub + 2) != 2) continue;
                sub += ((size_t)u16(sub + 4) << 16) | u16(sub + 6);
            } else if (type != 2) {
                continue;
            }
            impl->gposKern.push_back(sub);
        }
    }
}

/* The x-advance adjustment GPOS 'kern' gives the pair, in font units, and
 * whether any of its PairPos subtables covers the left glyph at all. */
int GposPairAdvance(const FontFaceImpl* impl, FT_UInt left, FT_UInt right)
{
    const std::vector<unsigned char>& t = impl->gpos;
    auto u16 = [&](size_t o) -> unsigned {
        return o + 2 <= t.size() ? (unsigned)((t[o] << 8) | t[o + 1]) : 0u;
    };
    /* Index of `g` in a Coverage table, or -1. */
    auto coverage = [&](size_t at, unsigned g) -> int {
        const unsigned format = u16(at), count = u16(at + 2);
        if (format == 1) {
            for (unsigned i = 0; i < count; ++i)
                if (u16(at + 4 + 2 * i) == g) return (int)i;
        } else if (format == 2) {
            for (unsigned i = 0; i < count; ++i) {
                const size_t r = at + 4 + 6 * i;
                if (g >= u16(r) && g <= u16(r + 2)) return (int)(u16(r + 4) + g - u16(r));
            }
        }
        return -1;
    };
    auto classOf = [&](size_t at, unsigned g) -> unsigned {
        const unsigned format = u16(at);
        if (format == 1) {
            const unsigned first = u16(at + 2), count = u16(at + 4);
            return (g >= first && g < first + count) ? u16(at + 6 + 2 * (g - first)) : 0u;
        }
        if (format == 2) {
            const unsigned count = u16(at + 2);
            for (unsigned i = 0; i < count; ++i) {
                const size_t r = at + 4 + 6 * i;
                if (g >= u16(r) && g <= u16(r + 2)) return u16(r + 4);
            }
        }
        return 0u;
    };
    auto valueSize = [](unsigned format) -> size_t {
        size_t n = 0;
        for (; format; format >>= 1) n += format & 1;
        return 2 * n;
    };
    /* XAdvance of a ValueRecord, or 0 when the format has none. */
    auto xAdvance = [&](size_t at, unsigned format) -> int {
        if (!(format & 0x4)) return 0;
        size_t o = at + ((format & 0x1) ? 2 : 0) + ((format & 0x2) ? 2 : 0);
        return (int16_t)u16(o);
    };

    int total = 0;
    for (size_t sub : impl->gposKern) {
        const unsigned format = u16(sub);
        const int index = coverage(sub + u16(sub + 2), left);
        if (index < 0) continue;
        const unsigned vf1 = u16(sub + 4), vf2 = u16(sub + 6);
        const size_t s1 = valueSize(vf1), s2 = valueSize(vf2);
        if (format == 1) {
            if ((unsigned)index >= u16(sub + 8)) continue;
            const size_t set = sub + u16(sub + 10 + 2 * index);
            const unsigned pairs = u16(set);
            for (unsigned i = 0; i < pairs; ++i) {
                const size_t rec = set + 2 + i * (2 + s1 + s2);
                if (u16(rec) == right) { total += xAdvance(rec + 2, vf1); break; }
            }
        } else if (format == 2) {
            const unsigned c1 = classOf(sub + u16(sub + 8), left);
            const unsigned c2 = classOf(sub + u16(sub + 10), right);
            const unsigned n1 = u16(sub + 12), n2 = u16(sub + 14);
            if (c1 < n1 && c2 < n2)
                total += xAdvance(sub + 16 + (c1 * n2 + c2) * (s1 + s2), vf1);
        }
    }
    return total;
}

/* Splits a run into font segments the way the OpenType shaper behind GDI's
 * glyph fallback does: each glyph belongs to the face that draws it, except
 * that whitespace stays with the segment before it. Kerning happens only
 * inside a segment, and under \frs a space after a fallback glyph is spaced
 * like one. Measured: in Arial with SimSun fallback, "A 中" kerns A-space,
 * "中 A" kerns neither pair. */
struct RunSegments
{
    const FontFaceImpl* base;
    bool fallback;            // the run needs glyph fallback at all
    const FontFaceImpl* seg = nullptr;
    GlyphSource prev;

    RunSegments(const FontFaceImpl* b, bool f) : base(b), fallback(f) {}

    static bool IsSpace(uint32_t cp)
    {
        return cp == 0x20 || cp == 0xA0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A);
    }

    /* Advances to `src`; returns the kerning (pixels) that goes before it. */
    int Next(const GlyphSource& src);

    bool InBase() const { return seg == base; }
};

/* The kerning GDI gives a pair of neighbouring glyphs of one face in a run
 * that needs glyph fallback, in whole pixels: the face's GPOS 'kern'
 * feature, as an OpenType shaper applies it, or its old 'kern' table when
 * it has no GPOS kerning. */
int KernPixels(const FontFaceImpl* impl, FT_UInt left, FT_UInt right)
{
    if (!left || !right) return 0;
    if (!impl->gposRead) ReadGposKern(impl);
    FT_Pos units = 0;
    if (impl->gposHasKern) {
        units = GposPairAdvance(impl, left, right);
    } else if (FT_HAS_KERNING(impl->face)) {
        FT_Vector v;
        if (FT_Get_Kerning(impl->face, left, right, FT_KERNING_UNSCALED, &v) == 0) units = v.x;
    }
    return units ? Round26_6(FT_MulFix(units, impl->face->size->metrics.x_scale)) : 0;
}

int RunSegments::Next(const GlyphSource& src)
{
    const FontFaceImpl* const prevSeg = seg;
    seg = (IsSpace(src.codePoint) && prevSeg) ? prevSeg : src.impl;
    int kern = 0;
    if (fallback && prev.impl == src.impl && prevSeg == seg)
        kern = KernPixels(src.impl, prev.glyph, src.glyph);
    prev = src;
    return kern;
}

struct RotatedGlyph
{
    double width;     // w, the rounded advance
    double up;        // half-chord above the baseline
    double down;      // half-chord below
    double inkLeft;   // black box along the baseline, from the origin
    double inkRight;
    /* The half-chords that place the pivot. They differ from up and down
     * only for a simulated bold glyph, which GDI places by its plain ink. */
    double pivotUp;
    double pivotDown;

    /* From the pen to the pivot: the half that faces left on screen. */
    double Left(double sinA) const { return sinA >= 0.0 ? pivotUp : pivotDown; }
};

/* `c` and `s` are |cos| and |sin| of the orientation. */
RotatedGlyph MeasureRotated(const FontFaceImpl* impl, double c, double s, double ascent)
{
    const FT_GlyphSlot slot = impl->face->glyph;
    RotatedGlyph g;
    g.width = (double)Round26_6(slot->advance.x);

    double top = 0.0, bottom = 0.0, boldTop = 0.0;
    if (impl->lastEmboldened) {
        /* GDI measures a simulated bold glyph by its plain ink box grown by
         * the bold strengths -- Sy at the top, Sx at the right, neither
         * shrunk nor stretched for the angle -- not by the emboldened
         * outline; its advance has already grown by the pixel. Probed on
         * SimSun glyphs falling back from bold Arial at 30 and 45 degrees:
         * the extent's cx grows by exactly Sx, the steps match to a pixel. */
        const FT_BBox& box = impl->lastPlainBox;
        const int sy = (impl->emSize + 49) / 50 - 1;
        top = box.yMax / 64.0;
        boldTop = sy;
        bottom = box.yMin / 64.0;
        g.inkLeft = std::floor(box.xMin / 64.0);
        g.inkRight = std::ceil(box.xMax / 64.0) + sy + 1;
    } else if (slot->outline.n_points > 0) {
        FT_BBox box;
        FT_Outline_Get_BBox(&slot->outline, &box);
        top = box.yMax / 64.0;
        bottom = box.yMin / 64.0;
        /* GDI's black box is whole pixels, rounded outwards. */
        g.inkLeft = std::floor(box.xMin / 64.0);
        g.inkRight = std::ceil(box.xMax / 64.0);
    } else {
        g.inkLeft = 0.0;
        g.inkRight = 1.0;
    }

    const double half = g.width / 2.0;
    const double h2 = ascent / 16.0;   // H/2, H = ascent / 8
    auto radius = [&](double a, double b) {
        if (a <= 0.0 || b <= 0.0) return 0.0;
        return 1.0 / std::sqrt((c / a) * (c / a) + (s / b) * (s / b));
    };
    const double above = top > 0.0 ? top : 0.0;
    g.up = radius(half, h2 + above + boldTop);
    g.down = radius(half, h2 - (bottom < 0.0 ? bottom : 0.0));
    g.pivotUp = boldTop ? radius(half, h2 + above) : g.up;
    g.pivotDown = g.down;
    return g;
}

} // namespace

// ---------------------------------------------------------------------------
// Vertical ('@') fonts
// ---------------------------------------------------------------------------
//
// A face name with a leading '@' asks GDI for the face's vertical variant.
// Drawn on a horizontal run, as VSFilter draws everything, it keeps the plain
// face's metrics, advances and extents, and leaves Latin text alone; the CJK
// characters are laid on their side, a quarter turn counter-clockwise, in the
// forms the font's GSUB 'vert' feature gives them. Probed against GDI on
// SimSun, SimHei, KaiTi, FangSong, DengXian, Microsoft YaHei, MS Gothic, Yu
// Gothic and Malgun Gothic, a turned glyph point (x, y) lands at
//
//     x' = pen + (vertical origin - y)       vertical origin = yMax + vmtx TSB
//     y' = (usWinAscent + |sTypoDescender|) - x      from the top of the cell
//
// so the glyph's vertical origin, the top of its vertical em box, meets the
// pen, and its left edge sits a typo-descender below the baseline. A turned
// glyph steps by its vertical advance (vmtx), in the path and in
// GetTextExtentPoint32 alike -- a proportional pinyin letter or Greek letter
// takes a full em.

namespace {

/* The characters an '@' face turns: those that are double-byte in its code
 * page. Measured over the BMP on SimSun, Microsoft YaHei, SimHei, DengXian,
 * KaiTi and FangSong -- GBK, so pinyin letters, Greek and Cyrillic turn too --
 * with the whole Han and Hangul blocks and the Hangul jamo added, which Malgun
 * Gothic turns. Japanese and Korean faces turn a slightly different set: they
 * leave Greek and Cyrillic upright. */
bool TurnsInVerticalFont(uint32_t cp)
{
    static const uint32_t kRanges[][2] = {
        { 0x00A2, 0x00A5 }, { 0x00A7, 0x00A8 }, { 0x00AF, 0x00B1 },
        { 0x00B4, 0x00B5 }, { 0x00B7, 0x00B7 }, { 0x00C0, 0x00C1 },
        { 0x00C8, 0x00CA }, { 0x00CC, 0x00CD }, { 0x00D2, 0x00D3 },
        { 0x00D7, 0x00D7 }, { 0x00D9, 0x00DA }, { 0x00DC, 0x00DC },
        { 0x00E0, 0x00E1 }, { 0x00E8, 0x00EA }, { 0x00EC, 0x00ED },
        { 0x00F2, 0x00F3 }, { 0x00F7, 0x00F7 }, { 0x00F9, 0x00FA },
        { 0x00FC, 0x00FC }, { 0x0100, 0x0101 }, { 0x0112, 0x0113 },
        { 0x011A, 0x011B }, { 0x012A, 0x012B }, { 0x0144, 0x0144 },
        { 0x0148, 0x0148 }, { 0x014C, 0x014D }, { 0x016A, 0x016B },
        { 0x01CD, 0x01DC }, { 0x0251, 0x0251 }, { 0x0261, 0x0261 },
        { 0x02C7, 0x02C7 }, { 0x02C9, 0x02CB }, { 0x02D9, 0x02D9 },
        { 0x0391, 0x03A1 }, { 0x03A3, 0x03A9 }, { 0x03B1, 0x03C1 },
        { 0x03C3, 0x03C9 }, { 0x0401, 0x0401 }, { 0x0410, 0x044F },
        { 0x0451, 0x0451 }, { 0x1100, 0x11FF }, { 0x2010, 0x2010 },
        { 0x2013, 0x2016 }, { 0x2018, 0x2022 }, { 0x2024, 0x2027 },
        { 0x2030, 0x2030 }, { 0x2032, 0x2037 }, { 0x2039, 0x203E },
        { 0x2044, 0x2044 }, { 0x2103, 0x2103 }, { 0x2105, 0x2105 },
        { 0x2109, 0x2109 }, { 0x2113, 0x2113 }, { 0x2116, 0x2117 },
        { 0x2121, 0x2122 }, { 0x2126, 0x2126 }, { 0x212E, 0x212E },
        { 0x214D, 0x214E }, { 0x2160, 0x216B }, { 0x2170, 0x2179 },
        { 0x2190, 0x2193 }, { 0x2196, 0x2199 }, { 0x2208, 0x2208 },
        { 0x220F, 0x220F }, { 0x2211, 0x2211 }, { 0x2215, 0x2215 },
        { 0x2219, 0x221A }, { 0x221D, 0x2220 }, { 0x2223, 0x2223 },
        { 0x2225, 0x2225 }, { 0x2227, 0x222B }, { 0x222E, 0x222E },
        { 0x2234, 0x2237 }, { 0x223D, 0x223D }, { 0x2248, 0x2248 },
        { 0x224C, 0x224C }, { 0x2252, 0x2252 }, { 0x2260, 0x2261 },
        { 0x2264, 0x2267 }, { 0x226E, 0x226F }, { 0x2295, 0x2295 },
        { 0x2299, 0x2299 }, { 0x22A5, 0x22A5 }, { 0x22BF, 0x22BF },
        { 0x2312, 0x2312 }, { 0x2460, 0x2469 }, { 0x2474, 0x249B },
        { 0x2500, 0x254B }, { 0x2550, 0x2573 }, { 0x2581, 0x258F },
        { 0x2593, 0x2595 }, { 0x25A0, 0x25A1 }, { 0x25AA, 0x25AB },
        { 0x25B2, 0x25B3 }, { 0x25BC, 0x25BD }, { 0x25C6, 0x25C7 },
        { 0x25CA, 0x25CC }, { 0x25CE, 0x25CF }, { 0x25E2, 0x25E6 },
        { 0x2605, 0x2606 }, { 0x2609, 0x2609 }, { 0x2640, 0x2640 },
        { 0x2642, 0x2642 }, { 0x2E80, 0x2FDF }, { 0x3001, 0x303F },
        { 0x3041, 0x3096 }, { 0x3099, 0x30FF }, { 0x3105, 0x312D },
        { 0x3130, 0x318F }, { 0x3190, 0x319F }, { 0x31F0, 0x31FF },
        { 0x3220, 0x3231 }, { 0x3233, 0x3238 }, { 0x323A, 0x3243 },
        { 0x3250, 0x325F }, { 0x3280, 0x3296 }, { 0x3298, 0x3298 },
        { 0x329A, 0x32A3 }, { 0x32A9, 0x32CF }, { 0x3372, 0x3376 },
        { 0x3380, 0x3387 }, { 0x338A, 0x338C }, { 0x338E, 0x338F },
        { 0x3391, 0x3395 }, { 0x3399, 0x339A }, { 0x339C, 0x339E },
        { 0x33A1, 0x33A1 }, { 0x33A7, 0x33AF }, { 0x33B4, 0x33BA },
        { 0x33BC, 0x33C1 }, { 0x33C3, 0x33C7 }, { 0x33C9, 0x33CC },
        { 0x33CE, 0x33D7 }, { 0x33D9, 0x33DF }, { 0x3400, 0x4DBF },
        { 0x4E00, 0x9FFF }, { 0xAC00, 0xD7AF }, { 0xF900, 0xFAFF },
        { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE52 }, { 0xFE54, 0xFE57 },
        { 0xFE59, 0xFE66 }, { 0xFE68, 0xFE6B }, { 0xFF01, 0xFF60 },
        { 0xFFE0, 0xFFE6 }, { 0xFFE8, 0xFFEE }, { 0x20000, 0x3FFFF },
    };
    for (const auto& r : kRanges)
        if (cp >= r[0] && cp <= r[1]) return true;
    return false;
}

/* The single substitutions of the font's GSUB 'vert' feature ('vrt2' when
 * there is no 'vert'), glyph to glyph. Lookup types 1 and 7-wrapping-1 only;
 * that is all the vertical forms of the CJK fonts in use are made of. */
std::map<FT_UInt, FT_UInt> ReadVertForms(FT_Face face)
{
    std::map<FT_UInt, FT_UInt> out;
    const FT_ULong tag = FT_MAKE_TAG('G', 'S', 'U', 'B');
    FT_ULong len = 0;
    if (FT_Load_Sfnt_Table(face, tag, 0, nullptr, &len) != 0 || len < 10) return out;
    std::vector<unsigned char> t(len);
    if (FT_Load_Sfnt_Table(face, tag, 0, t.data(), &len) != 0) return out;

    auto u16 = [&](size_t o) -> unsigned {
        return o + 2 <= t.size() ? (unsigned)((t[o] << 8) | t[o + 1]) : 0u;
    };
    auto u32 = [&](size_t o) -> size_t { return ((size_t)u16(o) << 16) | u16(o + 2); };

    /* Calls f(glyph, coverage index) for every glyph a Coverage table lists. */
    auto coverage = [&](size_t at, const std::function<void(unsigned, unsigned)>& f) {
        const unsigned format = u16(at), count = u16(at + 2);
        if (format == 1) {
            for (unsigned i = 0; i < count && at + 4 + 2 * i + 2 <= t.size(); ++i)
                f(u16(at + 4 + 2 * i), i);
        } else if (format == 2) {
            for (unsigned i = 0; i < count && at + 4 + 6 * i + 6 <= t.size(); ++i) {
                const size_t r = at + 4 + 6 * i;
                const unsigned first = u16(r), last = u16(r + 2), index = u16(r + 4);
                for (unsigned g = first; g <= last && g - first < 0x10000; ++g)
                    f(g, index + (g - first));
            }
        }
    };

    auto singleSubst = [&](size_t at) {
        const unsigned format = u16(at);
        const size_t cov = at + u16(at + 2);
        if (format == 1) {
            const int delta = (int16_t)u16(at + 4);
            coverage(cov, [&](unsigned g, unsigned) { out[g] = (FT_UInt)((g + delta) & 0xFFFF); });
        } else if (format == 2) {
            const unsigned count = u16(at + 4);
            coverage(cov, [&](unsigned g, unsigned i) {
                if (i < count) out[g] = u16(at + 6 + 2 * i);
            });
        }
    };

    const size_t featureList = u16(6), lookupList = u16(8);
    std::vector<unsigned> lookups;
    for (const char* want : { "vert", "vrt2" }) {
        const unsigned features = u16(featureList);
        for (unsigned i = 0; i < features; ++i) {
            const size_t rec = featureList + 2 + 6 * i;
            if (rec + 6 > t.size() || std::memcmp(&t[rec], want, 4) != 0) continue;
            const size_t feature = featureList + u16(rec + 4);
            const unsigned n = u16(feature + 2);
            for (unsigned k = 0; k < n; ++k) lookups.push_back(u16(feature + 4 + 2 * k));
        }
        if (!lookups.empty()) break;
    }

    for (unsigned li : lookups) {
        if (li >= u16(lookupList)) continue;
        const size_t lookup = lookupList + u16(lookupList + 2 + 2 * li);
        const unsigned type = u16(lookup), subtables = u16(lookup + 4);
        for (unsigned k = 0; k < subtables; ++k) {
            size_t sub = lookup + u16(lookup + 6 + 2 * k);
            if (type == 7) {
                if (u16(sub) != 1 || u16(sub + 2) != 1) continue;
                sub += u32(sub + 4);
            } else if (type != 1) {
                continue;
            }
            singleSubst(sub);
        }
    }
    return out;
}

} // namespace

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

    PrepareFallbacks(str, len);
    std::lock_guard<std::mutex> g(Library::Get().lock);

    const bool fallback = RunUsesFallback(str, len);
    const bool keep = RunKeepsAdvances(str, len);

    if (m_impl->rotated && (!keep || fallback)) {
        const double c = m_impl->rotCos, s = m_impl->rotSin;
        const double ac = std::fabs(c), as = std::fabs(s);
        double pen = 0.0, firstPivot = 0.0, lastPivot = 0.0;
        double lo = 0.0, hi = 0.0;
        bool any = false;

        RunSegments segments(m_impl.get(), fallback);
        int i = 0;
        while (i < len) {
            const GlyphSource src = Resolve(NextCodePoint(str, len, i));
            const int kern = segments.Next(src);
            if (!src.impl->LoadGlyph(src.glyph)) continue;

            const RotatedGlyph rg = MeasureRotated(src.impl, ac, as, m_metrics.ascent);
            const bool plain = keep && segments.InBase();
            if (plain) pen += kern;
            const double pivot = pen + rg.Left(s);
            /* Along the rotated baseline, (cos a, -sin a) on screen, the
             * pivot has moved pivot * cos a; the ink box sits around it. */
            const double along = pivot * c - rg.width / 2.0;
            const double a0 = along + rg.inkLeft, a1 = along + rg.inkRight;
            if (!any) {
                lo = a0; hi = a1; firstPivot = pivot; any = true;
            } else {
                if (a0 < lo) lo = a0;
                if (a1 > hi) hi = a1;
            }
            lastPivot = pivot;
            pen += plain ? rg.width : rg.up + rg.down;
        }

        if (any) {
            extent->cx = (int)std::floor(hi - lo) + 1;
            if (!fallback)
                extent->cy = (int)std::lround(m_metrics.height + (lastPivot - firstPivot) * as);
        }
        return true;
    }

    int total = 0;
    int i = 0;
    RunSegments segments(m_impl.get(), fallback);
    while (i < len) {
        const GlyphSource src = Resolve(NextCodePoint(str, len, i));
        /* See "Glyph fallback": such a run is kerned. */
        total += segments.Next(src);
        if (!src.impl->LoadGlyph(src.glyph)) continue;
        const FT_GlyphSlot slot = src.impl->face->glyph;
        /* GDI rounds each advance before accumulating; matching that keeps
         * per-character and whole-run measurements consistent, which the
         * \fsp path in CText depends on. A glyph an '@' face turns on its
         * side steps by its vertical advance. */
        if (src.impl->vertical && TurnsInVerticalFont(src.codePoint))
            total += Round26_6(slot->metrics.vertAdvance);
        else
            total += Round26_6(slot->advance.x);
    }

    extent->cx = total;
    return true;
}

bool FontFace::AppendString(const wchar_t* str, int len, int dx, int dy, GlyphPath& path) const
{
    if (!m_impl || !str) return false;
    if (len < 0) len = (int)std::wcslen(str);

    PrepareFallbacks(str, len);
    std::lock_guard<std::mutex> g(Library::Get().lock);

    PathBuilder b;
    b.out = &path;
    b.originX = dx;
    b.originY = dy + m_metrics.ascent;
    b.penX = 0;
    b.contourOpen = false;
    b.cur.x = b.cur.y = 0;

    int runStart = (int)path.points.size();

    double rotatedPen = 0.0;
    const bool plainAdvances = m_impl->rotated && RunKeepsAdvances(str, len);
    const bool fallback = RunUsesFallback(str, len);
    RunSegments segments(m_impl.get(), fallback);
    if (m_impl->rotated) {
        b.rotate = true;
        b.originY = dy;   // the pivots sit on the TA_TOP line
        b.ascent = m_metrics.ascent;
        b.cosA = m_impl->rotCos;
        b.sinA = m_impl->rotSin;
    }

    int i = 0;
    while (i < len) {
        const GlyphSource src = Resolve(NextCodePoint(str, len, i));
        const FontFaceImpl* impl = src.impl;
        const int kern = segments.Next(src);
        if (!impl->LoadGlyph(src.glyph)) continue;

        if (m_impl->rotated) {
            const RotatedGlyph rg = MeasureRotated(impl, std::fabs(b.cosA),
                                                   std::fabs(b.sinA), m_metrics.ascent);
            const bool plain = plainAdvances && segments.InBase();
            if (plain) rotatedPen += kern;
            b.halfWidth = rg.width / 2.0;
            b.pivotX = dx + rotatedPen + rg.Left(b.sinA);
            FT_Outline_Decompose(&impl->face->glyph->outline, &kOutlineFuncs, &b);
            b.CloseContour();
            rotatedPen += plain ? rg.width : rg.up + rg.down;
            continue;
        }

        b.penX += (FT_Pos)kern << 6;
        FT_Pos advance = (FT_Pos)Round26_6(impl->face->glyph->advance.x) << 6;

        b.sideways = false;
        if (impl->vertical && TurnsInVerticalFont(src.codePoint)) {
            advance = (FT_Pos)Round26_6(impl->face->glyph->metrics.vertAdvance) << 6;
            auto form = impl->vertForms.find(src.glyph);
            if (form != impl->vertForms.end() && !impl->LoadGlyph(form->second))
                impl->LoadGlyph(src.glyph);
            b.sideways = true;
            b.vertOriginY = impl->vertOriginY;
            b.vertBottom = impl->vertBottom;
            b.cellTop = dy;
        }

        FT_Outline_Decompose(&impl->face->glyph->outline, &kOutlineFuncs, &b);
        b.CloseContour();

        b.penX += advance;
    }

    /* Where a rotated font's rules go was not probed; they stay horizontal at
     * the rotated font's baseline, as long as the run. */
    if (m_impl->rotated) b.originY = dy + m_metrics.ascent;
    const int runWidth = m_impl->rotated ? (int)std::lround(rotatedPen) : Round26_6(b.penX);

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

    /* Glyph-fallback families found not to be installed, so each is looked
     * up once rather than once per face and character. */
    std::set<std::wstring> missingFamilies;

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
    return OpenFontImpl(desc, false);
}

std::shared_ptr<FontFace> FontEngine::OpenExactFont(const FontDesc& desc)
{
    return OpenFontImpl(desc, true);
}

std::shared_ptr<FontFace> FontEngine::OpenFontImpl(const FontDesc& desc, bool exactOnly)
{
    std::lock_guard<std::mutex> g(m_impl->lock);

    const Impl::Key key = Impl::MakeKey(desc);
    auto it = m_impl->cache.find(key);
    if (it != m_impl->cache.end()) {
        if (auto live = it->second.lock()) {
            if (exactOnly && !live->m_impl->exactFamily) return nullptr;
            return live;
        }
        m_impl->cache.erase(it);
    }

    FontMatch match;
    std::shared_ptr<FontData> blob;
    bool fromMemory = false;

    /* "@Face" is Face drawn vertically; the face itself is found by its
     * plain name, installed or embedded. */
    std::wstring family = desc.faceName;
    const bool vertical = !family.empty() && family[0] == L'@';
    if (vertical) family.erase(0, 1);

    /* Script-embedded fonts take priority over anything installed. */
    auto mem = m_impl->memoryFonts.find(ToLower(family));
    if (mem != m_impl->memoryFonts.end()) {
        blob = mem->second.data;
        match.faceIndex = mem->second.faceIndex;
        match.exactFamily = true;
        /* Whether bold/italic must be faked depends on what the attachment
         * already is; decided in Build, once the face is open. */
        fromMemory = true;
    } else {
        if (!FontProvider::Get().Match(family, desc.weight, desc.italic,
                                       desc.charSet, &match))
            return nullptr;
        if (exactOnly && !match.exactFamily) return nullptr;
        blob = m_impl->LoadBlob(match.path, match.faceIndex);
    }

    if (!blob) return nullptr;

    std::shared_ptr<FontFace> face = Build(desc, match, blob, fromMemory, vertical);
    if (face) m_impl->cache[key] = face;
    return face;
}

std::shared_ptr<FontFace> FontEngine::Build(const FontDesc& desc, FontMatch match,
                                            const std::shared_ptr<FontData>& blob,
                                            bool fromMemory, bool vertical)
{
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
        const int tenths = ((desc.orientation % 3600) + 3600) % 3600;
        impl->rotated = true;
        /* Exact on the axes, where the metrics rule changes too. */
        switch (tenths) {
        case 900:  impl->rotCos = 0.0;  impl->rotSin = 1.0;  break;
        case 1800: impl->rotCos = -1.0; impl->rotSin = 0.0;  break;
        case 2700: impl->rotCos = 0.0;  impl->rotSin = -1.0; break;
        default: {
            const double rad = tenths * 3.14159265358979323846 / 1800.0;
            impl->rotCos = std::cos(rad);
            impl->rotSin = std::sin(rad);
        }
        }
    }

    /* Together with \frs, GDI's vertical and rotated layouts would have to
     * be combined in a way nobody has probed; the rotated one wins. */
    if (vertical && !impl->rotated) {
        impl->vertical = true;
        std::lock_guard<std::mutex> lg(Library::Get().lock);
        impl->vertForms = ReadVertForms(impl->face);
    }

    impl->syntheticBold = match.syntheticBold;
    impl->syntheticItalic = match.syntheticItalic;
    impl->exactFamily = match.exactFamily;
    impl->underline = desc.underline;
    impl->strikeOut = desc.strikeOut;
    impl->data = blob;   // keeps the bytes alive for the FT_Face's lifetime

    if (!SetupSize(impl.get(), desc, &face->m_metrics))
        return nullptr;

    face->m_impl = std::move(impl);
    face->m_desc = desc;
    return face;
}

// ---------------------------------------------------------------------------
// Glyph fallback
// ---------------------------------------------------------------------------
//
// A face with no glyph for a character used to draw glyph 0, the .notdef
// box: "第二行" in Arial came out as three boxes. GDI never does that; it
// takes the glyph from another face. Probed on a Chinese Windows 11 by
// comparing GDI's own paths (BeginPath / TextOutW / GetPath) glyph for glyph
// against each installed face at every size:
//
//  * A face with a SystemLink entry in the registry (Segoe UI, Tahoma,
//    Microsoft Sans Serif, the CJK faces themselves) takes the glyph from the
//    first face of that list that has it: Microsoft YaHei for Han in Segoe
//    UI, SimSun in Tahoma.
//  * A face with none (Arial, Times New Roman, Verdana, Georgia, Comic Sans
//    MS) takes it from the system's associated DBCS font -- SimSun on a
//    Chinese system -- and then from SimSun's own SystemLink list, which is
//    where Hangul comes from: Malgun Gothic.
//  * The size depends on the fallback face. One whose OS/2 win cell is
//    exactly its em -- SimSun, NSimSun, MS Gothic, the older CJK faces with
//    no internal leading -- is drawn at the base face's em (lfHeight =
//    -(base em)). Any other -- Microsoft YaHei, Malgun Gothic -- gets the
//    base's lfHeight unchanged, i.e. the base's *cell* height, and comes out
//    smaller than the Latin text around it, as upstream shows.
//  * Either way the glyph sits on the base face's baseline and steps by its
//    own advance; the line keeps the base face's metrics.
//  * A run that needs a fallback glyph anywhere goes through an OpenType
//    shaper, and comes out kerned by each face's GPOS 'kern' feature between
//    neighbouring glyphs of that face -- which GDI otherwise never applies:
//    "Ta" in Times New Roman is 549 px wide at a 576 px cell, "Ta中" puts the
//    'a' 36 px further left and measures 36 px short. It is GPOS, not the old
//    'kern' table: Tahoma's 'kern' table kerns "Ta" but its GPOS does not,
//    and GDI does not; Segoe UI's two disagree, and GDI follows GPOS. The
//    shaper splits the run into one segment per face, whitespace staying
//    with the segment before it, and kerns only inside a segment.
//
// Checked exact, point for point, at cell heights 240, 400 and 480.
//
// So: the platform's link list for the base face first (only Windows has
// one), then a fixed list that starts with SimSun and Malgun Gothic, so a
// Chinese Windows renders as upstream does. The faces after those are what
// macOS and Linux have instead; with no GDI to copy they are sized at the
// base's em, so CJK text is as large as the Latin text it sits in. The
// platform's own fallback is the last resort, for anything else.

namespace {

/* How a fallback face is sized; see "Glyph fallback". */
enum class FallbackSize
{
    Gdi,   // the base's em if the face's cell is its em, else the base's cell
    Em,    // the base's em
};

struct FallbackFace
{
    const wchar_t* family;
    FallbackSize size;
};

const FallbackFace kFallbackFaces[] = {
    // Windows, in the order GDI was measured to use them.
    { L"SimSun",               FallbackSize::Gdi },
    { L"NSimSun",              FallbackSize::Gdi },
    { L"Malgun Gothic",        FallbackSize::Gdi },
    { L"Microsoft YaHei",      FallbackSize::Gdi },
    { L"Microsoft JhengHei",   FallbackSize::Gdi },
    { L"Yu Gothic",            FallbackSize::Gdi },
    { L"Meiryo",               FallbackSize::Gdi },
    { L"MS Gothic",            FallbackSize::Gdi },
    // macOS.
    { L"Songti SC",             FallbackSize::Em },
    { L"STSong",                FallbackSize::Em },
    { L"PingFang SC",           FallbackSize::Em },
    { L"Hiragino Sans GB",      FallbackSize::Em },
    { L"Apple SD Gothic Neo",   FallbackSize::Em },
    { L"Hiragino Sans",         FallbackSize::Em },
    { L"Heiti SC",              FallbackSize::Em },
    // Linux, and anywhere the Noto / Source Han families are installed.
    { L"Noto Serif CJK SC",     FallbackSize::Em },
    { L"Source Han Serif SC",   FallbackSize::Em },
    { L"Noto Serif SC",         FallbackSize::Em },
    { L"Noto Sans CJK SC",      FallbackSize::Em },
    { L"Source Han Sans SC",    FallbackSize::Em },
    { L"Noto Sans SC",          FallbackSize::Em },
    { L"Noto Sans CJK JP",      FallbackSize::Em },
    { L"Noto Sans CJK KR",      FallbackSize::Em },
    { L"Noto Sans CJK TC",      FallbackSize::Em },
    { L"WenQuanYi Zen Hei",     FallbackSize::Em },
    { L"WenQuanYi Micro Hei",   FallbackSize::Em },
    { L"Droid Sans Fallback",   FallbackSize::Em },
    { L"AR PL UMing CN",        FallbackSize::Em },
    { L"Arial Unicode MS",      FallbackSize::Em },
};

/* Characters that draw nothing: controls, zero-width and bidi marks,
 * variation selectors. A face may well lack them, and no other face would
 * draw anything better, so they never trigger a font lookup. */
bool NeedsNoFallback(uint32_t cp)
{
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0)
        || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E)
        || (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0xFE00 && cp <= 0xFE0F)
        || cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE0FFF);
}

} // namespace

void FontFace::PrepareFallbacks(const wchar_t* str, int len) const
{
    std::vector<uint32_t> missing;
    {
        std::lock_guard<std::mutex> lib(Library::Get().lock);
        std::lock_guard<std::mutex> fb(m_fallbackLock);
        int i = 0;
        while (i < len) {
            const uint32_t cp = NextCodePoint(str, len, i);
            if (NeedsNoFallback(cp) || m_impl->GlyphIndex(cp) != 0) continue;
            if (m_fallbacks.count(cp)) continue;
            if (std::find(missing.begin(), missing.end(), cp) == missing.end())
                missing.push_back(cp);
        }
    }

    /* Opening fonts takes the engine's lock and then FreeType's, so neither
     * of ours may be held here. */
    for (uint32_t cp : missing) {
        Fallback f;
        f.face = FontEngine::Get().FindFallback(m_desc, m_impl->emSize, cp, &f.glyph);
        std::lock_guard<std::mutex> fb(m_fallbackLock);
        m_fallbacks.emplace(cp, f);
    }
}

bool FontFace::RunUsesFallback(const wchar_t* str, int len) const
{
    int i = 0;
    while (i < len)
        if (Resolve(NextCodePoint(str, len, i)).impl != m_impl.get()) return true;
    return false;
}

GlyphSource FontFace::Resolve(uint32_t cp) const
{
    GlyphSource src;
    src.impl = m_impl.get();
    src.codePoint = cp;
    src.glyph = m_impl->GlyphIndex(cp);
    if (src.glyph != 0) return src;

    std::lock_guard<std::mutex> fb(m_fallbackLock);
    auto it = m_fallbacks.find(cp);
    if (it != m_fallbacks.end() && it->second.face) {
        src.impl = it->second.face->m_impl.get();
        src.glyph = it->second.glyph;
    }
    return src;
}

std::shared_ptr<FontFace> FontEngine::FindFallback(const FontDesc& base, int baseEm,
                                                   uint32_t cp, unsigned* glyph)
{
    *glyph = 0;

    std::wstring baseFamily = base.faceName;
    const bool vertical = !baseFamily.empty() && baseFamily[0] == L'@';
    if (vertical) baseFamily.erase(0, 1);
    baseFamily = ToLower(baseFamily);

    /* The fallback carries the base's weight and slant, so a bold italic line
     * stays bold italic, but not its rules: those belong to the run, which
     * the base face lays out. It keeps the base's orientation too, though the
     * base turns and places its glyphs: GDI emboldens a turned glyph by a
     * strength that depends on the angle (see "GDI's simulated bold"). */
    FontDesc d = base;
    d.underline = false;
    d.strikeOut = false;

    auto covers = [&](const std::shared_ptr<FontFace>& f) -> unsigned {
        if (!f) return 0;
        std::lock_guard<std::mutex> lg(Library::Get().lock);
        return f->m_impl->GlyphIndex(cp);
    };

    std::vector<FallbackFace> candidates;
    const std::vector<std::wstring> linked =
        FontProvider::Get().LinkedFamilies(baseFamily, base.weight >= FW_BOLD);
    for (const std::wstring& family : linked)
        candidates.push_back({ family.c_str(), FallbackSize::Gdi });
    for (const FallbackFace& ff : kFallbackFaces)
        candidates.push_back(ff);

    for (const FallbackFace& ff : candidates) {
        const std::wstring family = ff.family;
        if (ToLower(family) == baseFamily) continue;
        {
            std::lock_guard<std::mutex> g(m_impl->lock);
            if (m_impl->missingFamilies.count(family)) continue;
        }

        d.faceName = (vertical ? L"@" : L"") + family;
        d.height = (ff.size == FallbackSize::Em && base.height > 0) ? -baseEm : base.height;

        std::shared_ptr<FontFace> f = OpenExactFont(d);
        if (!f) {
            std::lock_guard<std::mutex> g(m_impl->lock);
            m_impl->missingFamilies.insert(family);
            continue;
        }
        const unsigned gi = covers(f);
        if (!gi) continue;

        if (ff.size == FallbackSize::Gdi && base.height > 0 && f->m_impl->cellIsEm) {
            d.height = -baseEm;
            f = OpenExactFont(d);
            if (!f) continue;
        }
        *glyph = gi;
        return f;
    }

    /* Last resort: whatever the platform itself would fall back to. */
    FontMatch match;
    if (!FontProvider::Get().MatchCodePoint(cp, base.weight, base.italic, &match))
        return nullptr;

    /* Cached under the file and face index the platform chose, which cannot
     * collide with a family name. */
    d.faceName = (vertical ? L"@" : L"") + vsfm::U82W(match.path.c_str(), match.path.size())
               + L"#" + std::to_wstring(match.faceIndex);
    d.height = base.height > 0 ? -baseEm : base.height;

    std::shared_ptr<FontFace> f;
    {
        std::lock_guard<std::mutex> g(m_impl->lock);
        const Impl::Key key = Impl::MakeKey(d);
        auto it = m_impl->cache.find(key);
        if (it != m_impl->cache.end()) f = it->second.lock();
        if (!f) {
            std::shared_ptr<FontData> blob = m_impl->LoadBlob(match.path, match.faceIndex);
            if (blob) f = Build(d, match, blob, false, vertical);
            if (f) m_impl->cache[key] = f;
        }
    }
    if (unsigned gi = covers(f)) {
        *glyph = gi;
        return f;
    }
    return nullptr;
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
    impl->cellIsEm = (winAscent + winDescent == (int)f->units_per_EM);

    const FT_Fixed yScale = f->size->metrics.y_scale;

    metrics->ascent  = Round26_6(FT_MulFix(winAscent, yScale));
    metrics->descent = Round26_6(FT_MulFix(winDescent, yScale));

    if (impl->vertical) {
        const int typoDescent = (os2 && os2->version != 0xFFFF) ? std::abs(os2->sTypoDescender)
                                                               : -f->descender;
        impl->vertBottom = FT_MulFix(winAscent + typoDescent, yScale);
    }

    /* A rotated font reports its own TEXTMETRIC; see "GDI's layout of a
     * rotated font". At 180 degrees it keeps the unrotated one. */
    if (impl->rotated && impl->rotSin != 0.0) {
        int a = winAscent, d = winDescent;
        if (impl->rotCos != 0.0) {
            const TT_Header* head = (const TT_Header*)FT_Get_Sfnt_Table(f, FT_SFNT_HEAD);
            a = head ? head->yMax : (int)f->bbox.yMax;
            d = head ? -head->yMin : -(int)f->bbox.yMin;
        }
        const int pad = f->units_per_EM / 64;
        metrics->ascent  = (int)((FT_MulFix(a + pad, yScale) + 63) >> 6);
        metrics->descent = (int)((FT_MulFix(d + pad, yScale) + 63) >> 6);
    }
    metrics->height  = metrics->ascent + metrics->descent;
    metrics->internalLeading = metrics->height - emPixels;
    if (metrics->internalLeading < 0) metrics->internalLeading = 0;
    metrics->externalLeading = 0;

    /* post-table rule geometry, in the same y-down path units. */
    int upos = f->underline_position;
    int uthick = f->underline_thickness;
    if (uthick <= 0) uthick = f->units_per_EM / 14;

    /* FreeType reports underline_position as the *centre* of the stem: it has
     * already subtracted half the thickness from the post table's value when
     * loading the face. AppendRule wants the top edge, so step back up by half
     * a thickness. Getting this wrong puts every underline exactly half a
     * stroke too low compared to GDI -- measured at 2px on a 48px line. */
    impl->underlinePos = Round26_6(FT_MulFix(-upos - uthick / 2, yScale));
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
