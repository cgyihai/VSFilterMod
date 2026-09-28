/*
 *  VSFilterMod cross-platform port -- code-page conversion helpers.
 *
 *  The charset -> code page table is used on every platform, including Windows.
 *  The original called TranslateCharsetInfo(), which consults the machine's
 *  installed fonts and locale; a fixed table means a .ass file with
 *  "Encoding: 134" decodes identically everywhere, which matters as soon as
 *  BDN+XML output is expected to be reproducible across build hosts.
 */

#include "text_compat.h"

#if !defined(_WIN32)
#include <iconv.h>
#include <errno.h>
#endif

DWORD CharSetToCodePage(DWORD dwCharSet)
{
    switch (dwCharSet) {
    case CP_UTF8:              return CP_UTF8;
    case CP_UTF7:              return CP_UTF7;

    case ANSI_CHARSET:         return 1252;
    case DEFAULT_CHARSET:      return CP_ACP;
    case SYMBOL_CHARSET:       return 1252;
    case MAC_CHARSET:          return 10000;
    case SHIFTJIS_CHARSET:     return 932;
    case HANGUL_CHARSET:       return 949;
    case JOHAB_CHARSET:        return 1361;
    case GB2312_CHARSET:       return 936;
    case CHINESEBIG5_CHARSET:  return 950;
    case GREEK_CHARSET:        return 1253;
    case TURKISH_CHARSET:      return 1254;
    case VIETNAMESE_CHARSET:   return 1258;
    case HEBREW_CHARSET:       return 1255;
    case ARABIC_CHARSET:       return 1256;
    case BALTIC_CHARSET:       return 1257;
    case RUSSIAN_CHARSET:      return 1251;
    case THAI_CHARSET:         return 874;
    case EASTEUROPE_CHARSET:   return 1250;
    case OEM_CHARSET:          return CP_ACP;

    default:
        /* Already a code page (the .ass "Encoding" field is sometimes abused
         * that way) -- pass it through. */
        return dwCharSet;
    }
}

#if !defined(_WIN32)

static_assert(sizeof(wchar_t) == 4,
              "the POSIX build assumes 32-bit wchar_t (UTF-32)");

static const char* CodePageToIconvName(UINT cp)
{
    switch (cp) {
    case CP_UTF8:  return "UTF-8";
    case CP_UTF7:  return "UTF-7";
    case CP_ACP:   return "UTF-8";   // modern POSIX default
    case 874:      return "CP874";
    case 932:      return "CP932";
    case 936:      return "CP936";
    case 949:      return "CP949";
    case 950:      return "CP950";
    case 1250:     return "CP1250";
    case 1251:     return "CP1251";
    case 1252:     return "CP1252";
    case 1253:     return "CP1253";
    case 1254:     return "CP1254";
    case 1255:     return "CP1255";
    case 1256:     return "CP1256";
    case 1257:     return "CP1257";
    case 1258:     return "CP1258";
    case 1361:     return "JOHAB";
    case 10000:    return "MACINTOSH";
    case 20127:    return "ASCII";
    case 28591:    return "ISO-8859-1";
    case 28592:    return "ISO-8859-2";
    case 28595:    return "ISO-8859-5";
    case 28597:    return "ISO-8859-7";
    case 28598:    return "ISO-8859-8";
    case 28599:    return "ISO-8859-9";
    case 28605:    return "ISO-8859-15";
    default:       return "CP1252";
    }
}

static const char* WideIconvName()
{
    const uint32_t probe = 1;
    return (*(const unsigned char*)&probe) ? "UTF-32LE" : "UTF-32BE";
}

/* Converts with iconv, transliterating anything unmappable to '?' so that a
 * single bad byte never truncates a subtitle line. Returns the number of
 * destination units written (excluding any terminator), or 0 on failure. */
static size_t IconvRun(const char* toCode, const char* fromCode,
                       const char* src, size_t srcBytes,
                       char* dst, size_t dstBytes, size_t dstUnitSize,
                       bool measureOnly)
{
    iconv_t cd = iconv_open(toCode, fromCode);
    if (cd == (iconv_t)-1) return 0;

    /* Scratch buffer for the measuring pass. */
    char scratch[1024];
    size_t written = 0;

    char* inPtr = const_cast<char*>(src);
    size_t inLeft = srcBytes;

    char* outPtr = measureOnly ? scratch : dst;
    size_t outLeft = measureOnly ? sizeof(scratch) : dstBytes;

    while (inLeft > 0) {
        char* outStart = outPtr;
        size_t rc = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
        written += (size_t)(outPtr - outStart);

        if (rc != (size_t)-1) break;

        if (errno == E2BIG) {
            if (!measureOnly) break;          // caller's buffer is full
            outPtr = scratch;                  // measuring: recycle scratch
            outLeft = sizeof(scratch);
            continue;
        }
        if (errno == EILSEQ || errno == EINVAL) {
            /* Skip one input byte and emit a replacement. */
            if (inLeft == 0) break;
            ++inPtr;
            --inLeft;
            if (outLeft >= dstUnitSize) {
                if (dstUnitSize == 4) {
                    uint32_t q = (uint32_t)'?';
                    std::memcpy(outPtr, &q, 4);
                } else {
                    *outPtr = '?';
                }
                outPtr += dstUnitSize;
                outLeft -= dstUnitSize;
                written += dstUnitSize;
            } else if (measureOnly) {
                written += dstUnitSize;
            }
            if (errno == EINVAL) break;        // truncated tail, nothing left
            continue;
        }
        break;
    }

    iconv_close(cd);
    return written / dstUnitSize;
}

int MultiByteToWideChar(UINT codePage, DWORD /*flags*/, const char* mb, int cbMultiByte,
                        wchar_t* wide, int cchWideChar)
{
    if (!mb) return 0;

    bool nulTerminated = (cbMultiByte < 0);
    size_t srcBytes = nulTerminated ? std::strlen(mb) : (size_t)cbMultiByte;

    size_t units = IconvRun(WideIconvName(), CodePageToIconvName(codePage),
                            mb, srcBytes,
                            (char*)wide, (size_t)cchWideChar * sizeof(wchar_t),
                            sizeof(wchar_t), cchWideChar == 0);

    if (cchWideChar == 0)
        return (int)units + (nulTerminated ? 1 : 0);

    /* Win32 appends the terminator only when the source was NUL terminated. */
    if (nulTerminated && wide && (int)units < cchWideChar) {
        wide[units] = L'\0';
        return (int)units + 1;
    }
    return (int)units;
}

int WideCharToMultiByte(UINT codePage, DWORD /*flags*/, const wchar_t* wide, int cchWideChar,
                        char* mb, int cbMultiByte, const char* /*defaultChar*/, BOOL* usedDefault)
{
    if (usedDefault) *usedDefault = FALSE;
    if (!wide) return 0;

    bool nulTerminated = (cchWideChar < 0);
    size_t srcUnits = nulTerminated ? std::wcslen(wide) : (size_t)cchWideChar;

    size_t bytes = IconvRun(CodePageToIconvName(codePage), WideIconvName(),
                            (const char*)wide, srcUnits * sizeof(wchar_t),
                            mb, (size_t)cbMultiByte,
                            1, cbMultiByte == 0);

    if (cbMultiByte == 0)
        return (int)bytes + (nulTerminated ? 1 : 0);

    if (nulTerminated && mb && (int)bytes < cbMultiByte) {
        mb[bytes] = '\0';
        return (int)bytes + 1;
    }
    return (int)bytes;
}

#endif // !_WIN32

CStringW UTF8To16(LPCSTR utf8)
{
    if (!utf8) return CStringW();
    return CStringW(vsfm::U82W(utf8, std::strlen(utf8)));
}

CStringA UTF16To8(LPCWSTR utf16)
{
    if (!utf16) return CStringA();
    return CStringA(vsfm::W2U8(utf16, std::wcslen(utf16)));
}

// ---------------------------------------------------------------------------

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

CStringW HostExecutablePath()
{
#if defined(_WIN32)
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return CStringW();
        if (n < buf.size()) return CStringW(buf.data(), (int)n);
        buf.resize(buf.size() * 2);          // truncated; try again
    }

#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);    // asks for the required length
    if (size == 0) return CStringW();

    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return CStringW();
    return CStringW(vsfm::U82W(buf.data(), std::strlen(buf.data())));

#else
    /* /proc/self/exe is Linux; readlink does not NUL terminate. */
    std::vector<char> buf(1024);
    for (;;) {
        const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return CStringW();
        if ((size_t)n < buf.size())
            return CStringW(vsfm::U82W(buf.data(), (size_t)n));
        buf.resize(buf.size() * 2);
    }
#endif
}

// ---------------------------------------------------------------------------

namespace {
thread_local unsigned int g_randState = 1;   // the CRT's initial seed
}

void vsfm_srand(unsigned int seed)
{
    g_randState = seed;
}

int vsfm_rand()
{
    g_randState = g_randState * 214013u + 2531011u;
    return (int)((g_randState >> 16) & VSFM_RAND_MAX);
}
