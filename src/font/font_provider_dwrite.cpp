/*
 *  VSFilterMod cross-platform port -- DirectWrite font lookup (Windows).
 *
 *  Only lookup: the outlines still come from FreeType, so a script renders the
 *  same on all three platforms. DirectWrite is used because it exposes the
 *  font *file* behind a family name, which GDI does not.
 */

#if defined(VSFM_FONT_DWRITE)

#include "font_provider.h"
#include "../compat/cstring_compat.h"

#include <dwrite.h>
#include <dwrite_2.h>

#include <algorithm>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "advapi32.lib")

namespace vsfm {

namespace {

template <typename T>
class ComPtr
{
public:
    ComPtr() : m_p(nullptr) {}
    ~ComPtr() { if (m_p) m_p->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** operator&() { return &m_p; }
    T* operator->() const { return m_p; }
    T* get() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    void reset() { if (m_p) { m_p->Release(); m_p = nullptr; } }

private:
    T* m_p;
};

DWRITE_FONT_WEIGHT ToDWriteWeight(int gdiWeight)
{
    if (gdiWeight < 1) return DWRITE_FONT_WEIGHT_NORMAL;
    if (gdiWeight > 999) return DWRITE_FONT_WEIGHT_BLACK;
    return (DWRITE_FONT_WEIGHT)gdiWeight;
}

/* A representative family per charset, used when the requested family is not
 * installed. DirectWrite's FindFamilyName is exact-match only, so without this
 * a missing Japanese family would silently fall through to a Latin face. */
const wchar_t* CharSetFallbackFamily(int charSet)
{
    switch (charSet) {
    case SHIFTJIS_CHARSET:    return L"Yu Gothic";
    case HANGUL_CHARSET:
    case JOHAB_CHARSET:       return L"Malgun Gothic";
    case GB2312_CHARSET:      return L"Microsoft YaHei";
    case CHINESEBIG5_CHARSET: return L"Microsoft JhengHei";
    case HEBREW_CHARSET:      return L"Segoe UI";
    case ARABIC_CHARSET:      return L"Segoe UI";
    case THAI_CHARSET:        return L"Leelawadee UI";
    default:                  return L"Arial";
    }
}

/* The one character MapCharacters is asked about. DirectWrite's fallback
 * reads its text through this interface; the object lives on the caller's
 * stack for the length of the call, so reference counting is a formality. */
class OneCharSource final : public IDWriteTextAnalysisSource
{
public:
    OneCharSource(const wchar_t* text, UINT32 len) : m_text(text), m_len(len) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSource)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, const WCHAR** text,
                                                UINT32* len) override
    {
        *text = pos < m_len ? m_text + pos : nullptr;
        *len = pos < m_len ? m_len - pos : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, const WCHAR** text,
                                                    UINT32* len) override
    {
        *text = pos > 0 && pos <= m_len ? m_text : nullptr;
        *len = pos > 0 && pos <= m_len ? pos : 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    /* No locale: the answer should not depend on the user's language. */
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32* len, const WCHAR** name) override
    {
        *len = pos < m_len ? m_len - pos : 0;
        *name = L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32* len,
                                                    IDWriteNumberSubstitution** sub) override
    {
        *len = pos < m_len ? m_len - pos : 0;
        *sub = nullptr;
        return S_OK;
    }

private:
    const wchar_t* m_text;
    UINT32 m_len;
};

class DWriteProvider final : public FontProvider
{
public:
    DWriteProvider()
    {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&m_factory));
        if (SUCCEEDED(hr) && m_factory) {
            m_factory->GetSystemFontCollection(&m_collection, FALSE);
            m_factory->GetGdiInterop(&m_gdiInterop);
        }
    }

    bool Match(const std::wstring& family, int weight, bool italic,
               int charSet, FontMatch* out) override
    {
        if (!out || !m_factory || !m_collection) return false;

        std::lock_guard<std::mutex> g(m_lock);

        /* A leading '@' asks GDI for the vertical variant of a CJK face.
         * FontEngine::OpenFont strips it before asking any provider and does
         * the vertical layout itself; this is a second guard, because
         * matching "@Microsoft YaHei" literally fails, falls back to Arial,
         * and every CJK character renders as a box. */
        std::wstring name = family;
        if (!name.empty() && name[0] == L'@') name.erase(0, 1);

        /* First choice: GDI's own matching rules, via DirectWrite's GDI
         * interop. FindFamilyName only knows DirectWrite's weight/stretch/
         * style families, so a GDI family name like "Segoe UI Semibold" or
         * "Arial Black" -- which DirectWrite files under "Segoe UI" and "Arial"
         * -- is not found and silently falls back to Arial. Scripts name fonts
         * the way GDI does, so match the way GDI does. */
        if (!name.empty() && m_gdiInterop && MatchLikeGdi(name, weight, italic, charSet, out))
            return true;

        UINT32 index = 0;
        BOOL exists = FALSE;

        if (!name.empty())
            m_collection->FindFamilyName(name.c_str(), &index, &exists);

        bool exact = (exists != FALSE);
        if (!exact) {
            const wchar_t* sub = CharSetFallbackFamily(charSet);
            m_collection->FindFamilyName(sub, &index, &exists);
            if (!exists) {
                m_collection->FindFamilyName(L"Arial", &index, &exists);
                if (!exists) return false;
            }
        }

        ComPtr<IDWriteFontFamily> fam;
        if (FAILED(m_collection->GetFontFamily(index, &fam)) || !fam) return false;

        ComPtr<IDWriteFont> font;
        if (FAILED(fam->GetFirstMatchingFont(
                ToDWriteWeight(weight),
                DWRITE_FONT_STRETCH_NORMAL,
                italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                &font)) || !font)
            return false;

        ComPtr<IDWriteFontFace> fontFace;
        if (FAILED(font->CreateFontFace(&fontFace)) || !fontFace) return false;

        if (!ResolveFile(fontFace.get(), out)) return false;

        out->faceIndex = (int)fontFace->GetIndex();
        out->exactFamily = exact;

        /* DirectWrite reports the style it actually landed on, so we only ask
         * FreeType to synthesise what really is missing. */
        out->syntheticBold = (weight >= FW_BOLD) &&
                             (font->GetWeight() < DWRITE_FONT_WEIGHT_SEMI_BOLD);
        out->syntheticItalic = italic &&
                               (font->GetStyle() == DWRITE_FONT_STYLE_NORMAL);
        return true;
    }

    std::vector<std::wstring> LinkedFamilies(const std::wstring& family, bool bold) override
    {
        std::vector<std::wstring> out;
        std::wstring name = family;
        if (!name.empty() && name[0] == L'@') name.erase(0, 1);
        if (name.empty()) return out;

        if (bold) ReadSystemLink(name + L" Bold", out);
        ReadSystemLink(name, out);
        return out;
    }

    bool MatchCodePoint(uint32_t cp, int weight, bool italic, FontMatch* out) override
    {
        if (!out || !m_factory || !m_collection) return false;

        std::lock_guard<std::mutex> g(m_lock);

        /* IDWriteFontFallback needs Windows 8.1; before that there is nothing
         * to ask, and the caller keeps the .notdef glyph. */
        if (!m_fallbackTried) {
            m_fallbackTried = true;
            ComPtr<IDWriteFactory2> factory2;
            if (SUCCEEDED(m_factory->QueryInterface(__uuidof(IDWriteFactory2),
                                                    reinterpret_cast<void**>(&factory2)))
                && factory2)
                factory2->GetSystemFontFallback(&m_fallback);
        }
        if (!m_fallback) return false;

        wchar_t text[2];
        UINT32 len = 1;
        if (cp >= 0x10000) {
            text[0] = (wchar_t)(0xD800 + ((cp - 0x10000) >> 10));
            text[1] = (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
            len = 2;
        } else {
            text[0] = (wchar_t)cp;
        }

        OneCharSource source(text, len);
        UINT32 mapped = 0;
        ComPtr<IDWriteFont> font;
        FLOAT scale = 1.0f;
        if (FAILED(m_fallback->MapCharacters(&source, 0, len, m_collection.get(), nullptr,
                                             ToDWriteWeight(weight),
                                             italic ? DWRITE_FONT_STYLE_ITALIC
                                                    : DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL,
                                             &mapped, &font, &scale))
            || !font || mapped == 0)
            return false;

        ComPtr<IDWriteFontFace> fontFace;
        if (FAILED(font->CreateFontFace(&fontFace)) || !fontFace) return false;
        if (!ResolveFile(fontFace.get(), out)) return false;

        out->faceIndex = (int)fontFace->GetIndex();
        out->exactFamily = true;
        out->syntheticBold = (weight >= FW_BOLD) &&
                             (font->GetWeight() < DWRITE_FONT_WEIGHT_SEMI_BOLD);
        out->syntheticItalic = italic &&
                               (font->GetStyle() == DWRITE_FONT_STYLE_NORMAL);
        return true;
    }

private:
    /* Appends the faces of one SystemLink value. Each entry is
     * "FILE.TTC,Face Name" with optional scaling numbers after it, which GDI
     * applies only to UI rendering at other DPIs; entries with no face name
     * are skipped, and a face listed twice is kept once. */
    static void ReadSystemLink(const std::wstring& name, std::vector<std::wstring>& out)
    {
        static const wchar_t kKey[] =
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink";
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, name.c_str(), RRF_RT_REG_MULTI_SZ,
                         nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
            return;
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
        if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, name.c_str(), RRF_RT_REG_MULTI_SZ,
                         nullptr, buf.data(), &bytes) != ERROR_SUCCESS)
            return;

        for (const wchar_t* e = buf.data(); *e; e += std::wcslen(e) + 1) {
            const wchar_t* comma = std::wcschr(e, L',');
            if (!comma || !comma[1]) continue;
            const wchar_t* end = std::wcschr(comma + 1, L',');
            std::wstring face = end ? std::wstring(comma + 1, end) : std::wstring(comma + 1);
            if (!face.empty() && std::find(out.begin(), out.end(), face) == out.end())
                out.push_back(face);
        }
    }

    bool MatchLikeGdi(const std::wstring& name, int weight, bool italic,
                      int charSet, FontMatch* out)
    {
        LOGFONTW lf;
        std::memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -100;                 // size is irrelevant to matching
        lf.lfWeight = weight;
        lf.lfItalic = italic ? TRUE : FALSE;
        lf.lfCharSet = (BYTE)charSet;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy(lf.lfFaceName, name.c_str(), LF_FACESIZE - 1);

        ComPtr<IDWriteFont> font;
        if (FAILED(m_gdiInterop->CreateFontFromLOGFONT(&lf, &font)) || !font)
            return false;               // not installed: caller falls back

        ComPtr<IDWriteFontFace> fontFace;
        if (FAILED(font->CreateFontFace(&fontFace)) || !fontFace) return false;
        if (!ResolveFile(fontFace.get(), out)) return false;

        out->faceIndex = (int)fontFace->GetIndex();
        out->exactFamily = true;

        /* GetSimulations reports exactly what GDI would fake for this LOGFONT,
         * which is a better answer than comparing weights ourselves. */
        const DWRITE_FONT_SIMULATIONS sim = font->GetSimulations();
        out->syntheticBold = (sim & DWRITE_FONT_SIMULATIONS_BOLD) != 0;
        out->syntheticItalic = (sim & DWRITE_FONT_SIMULATIONS_OBLIQUE) != 0;
        return true;
    }

    static bool ResolveFile(IDWriteFontFace* face, FontMatch* out)
    {
        UINT32 fileCount = 0;
        if (FAILED(face->GetFiles(&fileCount, nullptr)) || fileCount == 0) return false;

        std::vector<IDWriteFontFile*> files(fileCount, nullptr);
        if (FAILED(face->GetFiles(&fileCount, files.data()))) return false;

        bool ok = false;
        if (files[0]) {
            const void* refKey = nullptr;
            UINT32 refKeySize = 0;
            ComPtr<IDWriteFontFileLoader> loader;

            if (SUCCEEDED(files[0]->GetReferenceKey(&refKey, &refKeySize)) &&
                SUCCEEDED(files[0]->GetLoader(&loader)) && loader) {

                ComPtr<IDWriteLocalFontFileLoader> local;
                if (SUCCEEDED(loader->QueryInterface(
                        __uuidof(IDWriteLocalFontFileLoader),
                        reinterpret_cast<void**>(&local))) && local) {

                    UINT32 len = 0;
                    if (SUCCEEDED(local->GetFilePathLengthFromKey(refKey, refKeySize, &len))) {
                        std::wstring path(len + 1, L'\0');
                        if (SUCCEEDED(local->GetFilePathFromKey(refKey, refKeySize,
                                                                &path[0], len + 1))) {
                            path.resize(len);
                            out->path = vsfm::W2U8(path.c_str(), path.size());
                            ok = true;
                        }
                    }
                }
            }
        }

        for (auto* f : files) if (f) f->Release();
        return ok;
    }

    ComPtr<IDWriteFactory> m_factory;
    ComPtr<IDWriteFontCollection> m_collection;
    ComPtr<IDWriteGdiInterop> m_gdiInterop;
    ComPtr<IDWriteFontFallback> m_fallback;
    bool m_fallbackTried = false;
    std::mutex m_lock;
};

} // namespace

FontProvider& FontProvider::Get()
{
    static DWriteProvider inst;
    return inst;
}

} // namespace vsfm

#endif // VSFM_FONT_DWRITE
