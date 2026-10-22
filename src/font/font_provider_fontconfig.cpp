/*
 *  VSFilterMod cross-platform port -- fontconfig font lookup (Linux, BSD).
 */

#if defined(VSFM_FONT_FONTCONFIG)

#include "font_provider.h"
#include "../compat/cstring_compat.h"

#include <fontconfig/fontconfig.h>

#include <mutex>
#include <set>

namespace vsfm {

namespace {

/* Maps a Windows charset id to a representative code point, so fontconfig can
 * substitute a font that actually covers the script when the requested family
 * is missing. Without this a Japanese script with an unavailable family falls
 * back to a Latin-only font and renders as tofu. */
FcChar32 CharSetProbe(int charSet)
{
    switch (charSet) {
    case SHIFTJIS_CHARSET:    return 0x3042;  // HIRAGANA LETTER A
    case HANGUL_CHARSET:
    case JOHAB_CHARSET:       return 0xAC00;  // HANGUL SYLLABLE GA
    case GB2312_CHARSET:      return 0x4E00;  // CJK UNIFIED IDEOGRAPH-4E00
    case CHINESEBIG5_CHARSET: return 0x4E00;
    case GREEK_CHARSET:       return 0x03B1;
    case RUSSIAN_CHARSET:     return 0x0410;
    case HEBREW_CHARSET:      return 0x05D0;
    case ARABIC_CHARSET:      return 0x0627;
    case THAI_CHARSET:        return 0x0E01;
    default:                  return 0;
    }
}

int ToFcWeight(int gdiWeight)
{
#if FC_VERSION >= 21191
    return FcWeightFromOpenType(gdiWeight);
#else
    if (gdiWeight <= 100) return FC_WEIGHT_THIN;
    if (gdiWeight <= 200) return FC_WEIGHT_EXTRALIGHT;
    if (gdiWeight <= 300) return FC_WEIGHT_LIGHT;
    if (gdiWeight <= 400) return FC_WEIGHT_REGULAR;
    if (gdiWeight <= 500) return FC_WEIGHT_MEDIUM;
    if (gdiWeight <= 600) return FC_WEIGHT_DEMIBOLD;
    if (gdiWeight <= 700) return FC_WEIGHT_BOLD;
    if (gdiWeight <= 800) return FC_WEIGHT_EXTRABOLD;
    return FC_WEIGHT_BLACK;
#endif
}

class FontconfigProvider final : public FontProvider
{
public:
    FontconfigProvider() { m_ok = FcInit() != FcFalse; }

    bool Match(const std::wstring& family, int weight, bool italic,
               int charSet, FontMatch* out) override
    {
        if (!m_ok || !out) return false;

        const std::string name = vsfm::W2U8(family.c_str(), family.size());

        std::lock_guard<std::mutex> g(m_lock);

        FcPattern* pat = FcPatternCreate();
        if (!pat) return false;

        if (!name.empty())
            FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)name.c_str());

        FcPatternAddInteger(pat, FC_WEIGHT, ToFcWeight(weight));
        FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        FcPatternAddBool(pat, FC_OUTLINE, FcTrue);

        if (FcChar32 probe = CharSetProbe(charSet)) {
            FcCharSet* cs = FcCharSetCreate();
            FcCharSetAddChar(cs, probe);
            FcPatternAddCharSet(pat, FC_CHARSET, cs);
            FcCharSetDestroy(cs);
        }

        FcConfigSubstitute(nullptr, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);

        FcResult result = FcResultNoMatch;
        FcPattern* matched = FcFontMatch(nullptr, pat, &result);
        FcPatternDestroy(pat);

        if (!matched || result != FcResultMatch) {
            if (matched) FcPatternDestroy(matched);
            return false;
        }

        FcChar8* file = nullptr;
        if (FcPatternGetString(matched, FC_FILE, 0, &file) != FcResultMatch || !file) {
            FcPatternDestroy(matched);
            return false;
        }
        out->path = (const char*)file;

        int index = 0;
        FcPatternGetInteger(matched, FC_INDEX, 0, &index);
        out->faceIndex = index;

        /* Decide whether FreeType has to fake the style the caller asked for. */
        int gotWeight = FC_WEIGHT_REGULAR;
        FcPatternGetInteger(matched, FC_WEIGHT, 0, &gotWeight);
        out->syntheticBold = (weight >= FW_BOLD) && (gotWeight < FC_WEIGHT_DEMIBOLD);

        int gotSlant = FC_SLANT_ROMAN;
        FcPatternGetInteger(matched, FC_SLANT, 0, &gotSlant);
        out->syntheticItalic = italic && (gotSlant == FC_SLANT_ROMAN);

        FcChar8* gotFamily = nullptr;
        out->exactFamily = false;
        if (FcPatternGetString(matched, FC_FAMILY, 0, &gotFamily) == FcResultMatch && gotFamily)
            out->exactFamily = (name == (const char*)gotFamily);

        FcPatternDestroy(matched);
        return true;
    }

    bool MatchCodePoint(uint32_t cp, int weight, bool italic, FontMatch* out) override
    {
        if (!m_ok || !out) return false;

        std::lock_guard<std::mutex> g(m_lock);

        FcPattern* pat = FcPatternCreate();
        if (!pat) return false;

        FcCharSet* cs = FcCharSetCreate();
        FcCharSetAddChar(cs, cp);
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcCharSetDestroy(cs);
        FcPatternAddInteger(pat, FC_WEIGHT, ToFcWeight(weight));
        FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        FcPatternAddBool(pat, FC_OUTLINE, FcTrue);

        FcConfigSubstitute(nullptr, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);

        /* FcFontMatch weighs the charset against everything else and may
         * still hand back a face without the character; the sorted list is
         * walked for the first one that really has it. */
        FcResult result = FcResultNoMatch;
        FcFontSet* set = FcFontSort(nullptr, pat, FcTrue, nullptr, &result);
        bool found = false;
        for (int i = 0; set && i < set->nfont && !found; ++i) {
            FcPattern* cand = FcFontRenderPrepare(nullptr, pat, set->fonts[i]);
            if (!cand) continue;

            FcCharSet* has = nullptr;
            FcChar8* file = nullptr;
            if (FcPatternGetCharSet(cand, FC_CHARSET, 0, &has) == FcResultMatch && has
                && FcCharSetHasChar(has, cp)
                && FcPatternGetString(cand, FC_FILE, 0, &file) == FcResultMatch && file) {
                out->path = (const char*)file;
                int index = 0;
                FcPatternGetInteger(cand, FC_INDEX, 0, &index);
                out->faceIndex = index;

                int gotWeight = FC_WEIGHT_REGULAR;
                FcPatternGetInteger(cand, FC_WEIGHT, 0, &gotWeight);
                out->syntheticBold = (weight >= FW_BOLD) && (gotWeight < FC_WEIGHT_DEMIBOLD);
                int gotSlant = FC_SLANT_ROMAN;
                FcPatternGetInteger(cand, FC_SLANT, 0, &gotSlant);
                out->syntheticItalic = italic && (gotSlant == FC_SLANT_ROMAN);
                out->exactFamily = true;
                found = true;
            }
            FcPatternDestroy(cand);
        }
        if (set) FcFontSetDestroy(set);
        FcPatternDestroy(pat);
        return found;
    }

private:
    bool m_ok = false;
    std::mutex m_lock;
};

} // namespace

FontProvider& FontProvider::Get()
{
    static FontconfigProvider inst;
    return inst;
}

} // namespace vsfm

#endif // VSFM_FONT_FONTCONFIG
