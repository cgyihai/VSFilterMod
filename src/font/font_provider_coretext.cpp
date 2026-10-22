/*
 *  VSFilterMod cross-platform port -- CoreText font lookup (macOS).
 */

#if defined(VSFM_FONT_CORETEXT)

#include "font_provider.h"
#include "../compat/cstring_compat.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <mutex>
#include <string>
#include <cstring>
#include <climits>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

namespace vsfm {

namespace {

template <typename T>
class CFHandle
{
public:
    CFHandle() : m_ref(nullptr) {}
    explicit CFHandle(T ref) : m_ref(ref) {}
    ~CFHandle() { if (m_ref) CFRelease(m_ref); }
    CFHandle(const CFHandle&) = delete;
    CFHandle& operator=(const CFHandle&) = delete;

    T get() const { return m_ref; }
    void reset(T ref) { if (m_ref) CFRelease(m_ref); m_ref = ref; }
    explicit operator bool() const { return m_ref != nullptr; }

private:
    T m_ref;
};

std::string CFStringToUtf8(CFStringRef s)
{
    if (!s) return std::string();

    CFIndex len = CFStringGetLength(s);
    CFIndex cap = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out((size_t)cap, '\0');
    if (!CFStringGetCString(s, &out[0], cap, kCFStringEncodingUTF8))
        return std::string();
    out.resize(std::strlen(out.c_str()));
    return out;
}

CFStringRef MakeCFString(const std::string& utf8)
{
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   (const UInt8*)utf8.data(), (CFIndex)utf8.size(),
                                   kCFStringEncodingUTF8, false);
}

/* CoreText hands back a file URL but no face index, and most macOS system
 * fonts live in .ttc collections. Resolve the index by matching PostScript
 * names with FreeType; otherwise a request for "Hiragino Sans W3" would render
 * as whatever happens to sit at index 0 of the collection. */
int ResolveFaceIndex(const std::string& path, const std::string& postScriptName)
{
    if (postScriptName.empty()) return 0;

    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return 0;

    int found = 0;
    FT_Face probe = nullptr;
    if (FT_New_Face(lib, path.c_str(), -1, &probe) == 0) {
        const long count = probe->num_faces;
        FT_Done_Face(probe);

        for (long i = 0; i < count; ++i) {
            FT_Face f = nullptr;
            if (FT_New_Face(lib, path.c_str(), i, &f) != 0) continue;
            const char* ps = FT_Get_Postscript_Name(f);
            const bool hit = ps && postScriptName == ps;
            FT_Done_Face(f);
            if (hit) { found = (int)i; break; }
        }
    }

    FT_Done_FreeType(lib);
    return found;
}

class CoreTextProvider final : public FontProvider
{
public:
    bool Match(const std::wstring& family, int weight, bool italic,
               int charSet, FontMatch* out) override
    {
        if (!out) return false;

        const std::string name = vsfm::W2U8(family.c_str(), family.size());

        std::lock_guard<std::mutex> g(m_lock);

        CFHandle<CFMutableDictionaryRef> attrs(
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
        if (!attrs) return false;

        CFHandle<CFStringRef> cfFamily(name.empty() ? nullptr : MakeCFString(name));
        if (cfFamily)
            CFDictionarySetValue(attrs.get(), kCTFontFamilyNameAttribute, cfFamily.get());

        /* Ask for the symbolic traits rather than a weight number: CoreText's
         * own matching then picks the closest installed face. */
        CTFontSymbolicTraits symbolic = 0;
        if (weight >= FW_BOLD) symbolic |= kCTFontTraitBold;
        if (italic) symbolic |= kCTFontTraitItalic;

        CFHandle<CFMutableDictionaryRef> traits(
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
        if (traits && symbolic) {
            CFHandle<CFNumberRef> num(
                CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &symbolic));
            CFDictionarySetValue(traits.get(), kCTFontSymbolicTrait, num.get());
            CFDictionarySetValue(attrs.get(), kCTFontTraitsAttribute, traits.get());
        }

        CFHandle<CTFontDescriptorRef> desc(
            CTFontDescriptorCreateWithAttributes(attrs.get()));
        if (!desc) return false;

        CFHandle<CTFontRef> font(CTFontCreateWithFontDescriptor(desc.get(), 0.0, nullptr));
        if (!font) return false;

        /* When the family is missing entirely, or the script needs coverage the
         * matched face lacks, let CoreText pick a fallback for a probe string. */
        CTFontRef effective = font.get();
        CFHandle<CTFontRef> fallback;
        if (CFStringRef probe = CharSetProbeString(charSet)) {
            CFHandle<CFStringRef> probeHandle(probe);
            fallback.reset(CTFontCreateForString(
                font.get(), probe, CFRangeMake(0, CFStringGetLength(probe))));
            if (fallback) effective = fallback.get();
        }

        CFHandle<CFURLRef> url((CFURLRef)CTFontCopyAttribute(effective, kCTFontURLAttribute));
        if (!url) return false;

        char pathBuf[PATH_MAX];
        if (!CFURLGetFileSystemRepresentation(url.get(), true, (UInt8*)pathBuf, sizeof(pathBuf)))
            return false;
        out->path = pathBuf;

        CFHandle<CFStringRef> psName(CTFontCopyPostScriptName(effective));
        out->faceIndex = ResolveFaceIndex(out->path, CFStringToUtf8(psName.get()));

        const CTFontSymbolicTraits got = CTFontGetSymbolicTraits(effective);
        out->syntheticBold = (weight >= FW_BOLD) && !(got & kCTFontTraitBold);
        out->syntheticItalic = italic && !(got & kCTFontTraitItalic);

        CFHandle<CFStringRef> gotFamily(CTFontCopyFamilyName(effective));
        out->exactFamily = (CFStringToUtf8(gotFamily.get()) == name);

        return true;
    }

    bool MatchCodePoint(uint32_t cp, int weight, bool italic, FontMatch* out) override
    {
        if (!out) return false;

        std::lock_guard<std::mutex> g(m_lock);

        /* CoreText's cascade list, asked through a Latin face: which installed
         * face would draw this character in its place. */
        CFHandle<CFStringRef> baseName(MakeCFString("Helvetica"));
        CFHandle<CTFontRef> base(CTFontCreateWithName(baseName.get(), 12.0, nullptr));
        if (!base) return false;

        UniChar units[2];
        CFIndex count = 1;
        if (cp >= 0x10000) {
            units[0] = (UniChar)(0xD800 + ((cp - 0x10000) >> 10));
            units[1] = (UniChar)(0xDC00 + ((cp - 0x10000) & 0x3FF));
            count = 2;
        } else {
            units[0] = (UniChar)cp;
        }
        CFHandle<CFStringRef> str(CFStringCreateWithCharacters(kCFAllocatorDefault, units, count));
        if (!str) return false;

        CFHandle<CTFontRef> font(CTFontCreateForString(base.get(), str.get(),
                                                       CFRangeMake(0, count)));
        if (!font) return false;

        /* Asked about a character it cannot place either, CoreText hands the
         * base face back; FontEngine checks coverage, but skip the file load. */
        CGGlyph glyphs[2] = { 0, 0 };
        if (!CTFontGetGlyphsForCharacters(font.get(), units, glyphs, count)) return false;

        CFHandle<CFURLRef> url((CFURLRef)CTFontCopyAttribute(font.get(), kCTFontURLAttribute));
        if (!url) return false;

        char pathBuf[PATH_MAX];
        if (!CFURLGetFileSystemRepresentation(url.get(), true, (UInt8*)pathBuf, sizeof(pathBuf)))
            return false;
        out->path = pathBuf;

        CFHandle<CFStringRef> psName(CTFontCopyPostScriptName(font.get()));
        out->faceIndex = ResolveFaceIndex(out->path, CFStringToUtf8(psName.get()));

        const CTFontSymbolicTraits got = CTFontGetSymbolicTraits(font.get());
        out->syntheticBold = (weight >= FW_BOLD) && !(got & kCTFontTraitBold);
        out->syntheticItalic = italic && !(got & kCTFontTraitItalic);
        out->exactFamily = true;
        return true;
    }

private:
    /* Caller takes ownership of the returned string. */
    static CFStringRef CharSetProbeString(int charSet)
    {
        const char* probe = nullptr;
        switch (charSet) {
        case SHIFTJIS_CHARSET:    probe = "\xE3\x81\x82"; break;  // U+3042
        case HANGUL_CHARSET:
        case JOHAB_CHARSET:       probe = "\xEA\xB0\x80"; break;  // U+AC00
        case GB2312_CHARSET:
        case CHINESEBIG5_CHARSET: probe = "\xE4\xB8\x80"; break;  // U+4E00
        case GREEK_CHARSET:       probe = "\xCE\xB1";     break;
        case RUSSIAN_CHARSET:     probe = "\xD0\x90";     break;
        case HEBREW_CHARSET:      probe = "\xD7\x90";     break;
        case ARABIC_CHARSET:      probe = "\xD8\xA7";     break;
        case THAI_CHARSET:        probe = "\xE0\xB8\x81"; break;
        default: return nullptr;
        }
        return MakeCFString(probe);
    }

    std::mutex m_lock;
};

} // namespace

FontProvider& FontProvider::Get()
{
    static CoreTextProvider inst;
    return inst;
}

} // namespace vsfm

#endif // VSFM_FONT_CORETEXT
