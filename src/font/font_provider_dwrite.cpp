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

#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "dwrite.lib")

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

class DWriteProvider final : public FontProvider
{
public:
    DWriteProvider()
    {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&m_factory));
        if (SUCCEEDED(hr) && m_factory)
            m_factory->GetSystemFontCollection(&m_collection, FALSE);
    }

    bool Match(const std::wstring& family, int weight, bool italic,
               int charSet, FontMatch* out) override
    {
        if (!out || !m_factory || !m_collection) return false;

        std::lock_guard<std::mutex> g(m_lock);

        UINT32 index = 0;
        BOOL exists = FALSE;

        if (!family.empty())
            m_collection->FindFamilyName(family.c_str(), &index, &exists);

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

private:
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
