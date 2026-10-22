/*
 *  VSFilterMod cross-platform port -- font lookup.
 *
 *  Turning a family name ("MS PGothic"), a weight and an italic flag into a
 *  concrete font file is the one job that genuinely differs per platform.
 *  FreeType does the rest.
 *
 *  Backends: fontconfig (Linux/BSD), CoreText (macOS), DirectWrite (Windows).
 */

#pragma once

#include "../compat/platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vsfm {

struct FontMatch
{
    std::string path;        // UTF-8 path to the font file
    int  faceIndex = 0;
    bool syntheticBold = false;
    bool syntheticItalic = false;
    bool exactFamily = false;  // false when the platform substituted a family
};

class FontProvider
{
public:
    virtual ~FontProvider() {}

    /* `charSet` is the Windows charset id from the .ass style; providers use it
     * to pick a CJK-capable substitute when the requested family is missing. */
    virtual bool Match(const std::wstring& family, int weight, bool italic,
                       int charSet, FontMatch* out) = 0;

    /* Font data handed to us in memory (an .ass [Fonts] attachment) is kept by
     * the engine, not the provider, but the provider is asked first so a
     * platform-installed font of the same name does not shadow it. */
    virtual void AddMemoryFamily(const std::wstring& family) { (void)family; }

    /* The faces the platform links to `family` for characters it lacks, in
     * order: GDI's SystemLink registry list on Windows (the "Bold" entry
     * first for a bold face), nothing elsewhere. */
    virtual std::vector<std::wstring> LinkedFamilies(const std::wstring& family, bool bold)
    {
        (void)family; (void)bold;
        return std::vector<std::wstring>();
    }

    /* An installed face with a glyph for `cp`, chosen by the platform's own
     * fallback (DirectWrite's system fallback, CoreText's cascade list,
     * fontconfig's coverage sort). The last resort of FontEngine's glyph
     * fallback, after its fixed list of CJK faces. */
    virtual bool MatchCodePoint(uint32_t cp, int weight, bool italic, FontMatch* out)
    {
        (void)cp; (void)weight; (void)italic; (void)out;
        return false;
    }

    static FontProvider& Get();
};

} // namespace vsfm
