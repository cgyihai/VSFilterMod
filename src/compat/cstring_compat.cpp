/*
 *  VSFilterMod cross-platform port -- CString support routines.
 */

#include "cstring_compat.h"

namespace vsfm {

// ---------------------------------------------------------------------------
// UTF-8 <-> wchar_t. wchar_t is UTF-16 on Windows and UTF-32 elsewhere, so the
// surrogate handling is conditional but the UTF-8 side is identical.
// ---------------------------------------------------------------------------

static void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;

    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static void AppendWide(std::wstring& out, uint32_t cp)
{
    if (cp > 0x10FFFF) cp = 0xFFFD;

    if (sizeof(wchar_t) >= 4 || cp < 0x10000) {
        out += (wchar_t)cp;
    } else {
        cp -= 0x10000;
        out += (wchar_t)(0xD800 + (cp >> 10));
        out += (wchar_t)(0xDC00 + (cp & 0x3FF));
    }
}

std::string W2U8(const wchar_t* s, size_t len)
{
    std::string out;
    if (!s) return out;
    out.reserve(len + len / 2);

    for (size_t i = 0; i < len; ++i) {
        uint32_t cp = (uint32_t)s[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            uint32_t lo = (uint32_t)s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        AppendUtf8(out, cp);
    }
    return out;
}

std::wstring U82W(const char* s, size_t len)
{
    std::wstring out;
    if (!s) return out;
    out.reserve(len);

    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t extra;

        if (c < 0x80)            { cp = c;        extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { AppendWide(out, 0xFFFD); ++i; continue; }

        if (i + extra >= len) {        // truncated sequence at end of input
            AppendWide(out, 0xFFFD);
            break;
        }

        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { AppendWide(out, 0xFFFD); ++i; continue; }

        AppendWide(out, cp);
        i += extra + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Format-string dialect fixups.
//
// The sources were written against MSVC, where the wide printf family treats
// "%s" as wchar_t* and "%S" as char*, and where 64-bit values use "%I64d".
// C99 (glibc, libc++) inverts the first rule and does not know the second.
// ---------------------------------------------------------------------------

static bool StartsWith(const char* p, const char* lit)
{
    return std::strncmp(p, lit, std::strlen(lit)) == 0;
}

std::string NormalizeNarrowFormat(const char* fmt)
{
    std::string out;
    if (!fmt) return out;

#if defined(_WIN32)
    out = fmt;
    return out;
#else
    for (const char* p = fmt; *p; ) {
        if (*p != '%') { out += *p++; continue; }
        if (p[1] == '%') { out += "%%"; p += 2; continue; }

        const char* q = p + 1;
        while (*q && !std::strchr("diouxXeEfgGaAcspnSZ", *q)) {
            if (StartsWith(q, "I64")) break;
            ++q;
        }
        if (StartsWith(q, "I64")) {
            out.append(p, q - p);
            out += "ll";
            p = q + 3;
            continue;
        }
        if (*q == 'S') {              // MSVC: wide string in a narrow format
            out.append(p, q - p);
            out += "ls";
            p = q + 1;
            continue;
        }
        if (!*q) { out.append(p); break; }
        out.append(p, q - p + 1);
        p = q + 1;
    }
    return out;
#endif
}

std::wstring NormalizeWideFormat(const wchar_t* fmt)
{
    std::wstring out;
    if (!fmt) return out;

#if defined(_WIN32)
    out = fmt;
    return out;
#else
    auto starts_with = [](const wchar_t* p, const wchar_t* lit) {
        return std::wcsncmp(p, lit, std::wcslen(lit)) == 0;
    };

    for (const wchar_t* p = fmt; *p; ) {
        if (*p != L'%') { out += *p++; continue; }
        if (p[1] == L'%') { out += L"%%"; p += 2; continue; }

        const wchar_t* q = p + 1;
        while (*q && !std::wcschr(L"diouxXeEfgGaAcspnSZ", *q)) {
            if (starts_with(q, L"I64")) break;
            ++q;
        }
        if (starts_with(q, L"I64")) {
            out.append(p, q - p);
            out += L"ll";
            p = q + 3;
            continue;
        }
        if (*q == L's') {             // MSVC wide "%s" == wchar_t*
            out.append(p, q - p);
            out += L"ls";
            p = q + 1;
            continue;
        }
        if (*q == L'S') {             // MSVC wide "%S" == char*
            out.append(p, q - p);
            out += L"s";
            p = q + 1;
            continue;
        }
        if (*q == L'c') {             // likewise for characters
            out.append(p, q - p);
            out += L"lc";
            p = q + 1;
            continue;
        }
        if (!*q) { out.append(p); break; }
        out.append(p, q - p + 1);
        p = q + 1;
    }
    return out;
#endif
}

} // namespace vsfm
