/*
 *  VSFilterMod cross-platform port -- locale-independent number parsing.
 *  See num_compat.h.
 *
 *  Rather than switch locales (setlocale is process-wide and not ours to
 *  touch; the per-thread alternatives differ on every platform), the number
 *  is copied out with '.' replaced by whatever decimal point the current
 *  locale uses, and handed to the C library in that form. Its correctly
 *  rounded conversion is then used unchanged.
 */

#include "num_compat.h"

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

namespace vsfm {

namespace {

const char* LocaleDecimalPoint()
{
    const struct lconv* lc = std::localeconv();
    return (lc && lc->decimal_point && *lc->decimal_point) ? lc->decimal_point : ".";
}

bool IsSpace(unsigned c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/* The characters a C-locale strtod could consume: digits, letters (for the
 * exponent, hex digits, "inf", "nan"), signs and the point. */
bool IsNumberChar(unsigned c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c == '.' || c == '+' || c == '-';
}

/* Parses with `conv` (strtod or strtof) as if the locale's decimal point
 * were '.'. CharT is char or wchar_t. */
template <typename CharT, typename R>
R ParseReal(const CharT* s, CharT** end, R (*conv)(const char*, char**))
{
    const char* dp = LocaleDecimalPoint();
    const size_t dpLen = std::strlen(dp);

    size_t i = 0;
    while (IsSpace((unsigned)s[i])) ++i;

    std::string narrow;
    std::vector<size_t> origin;   // for each narrow char, its index in s
    for (; IsNumberChar((unsigned)s[i]); ++i) {
        if (s[i] == '.') {
            narrow.append(dp, dpLen);
            origin.insert(origin.end(), dpLen, i);
        } else {
            narrow += (char)s[i];
            origin.push_back(i);
        }
    }
    origin.push_back(i);

    char* e = nullptr;
    const R r = conv(narrow.c_str(), &e);
    const size_t used = (size_t)(e - narrow.c_str());

    /* No conversion: end is s itself, as the C functions do. The C library
     * takes a multi-byte decimal point whole or not at all, so `used` never
     * points into the middle of one. */
    if (end) *end = const_cast<CharT*>(used == 0 ? s : s + origin[used]);
    return r;
}

float StrtofAdapter(const char* s, char** e) { return std::strtof(s, e); }
double StrtodAdapter(const char* s, char** e) { return std::strtod(s, e); }

} // namespace

long WcsToL(const wchar_t* s, wchar_t** end, int base)
{
    const long long v = std::wcstoll(s, end, base);
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (long)v;
}

double WcsToD(const wchar_t* s, wchar_t** end) { return ParseReal(s, end, StrtodAdapter); }
float WcsToF(const wchar_t* s, wchar_t** end) { return ParseReal(s, end, StrtofAdapter); }
double StrToD(const char* s, char** end) { return ParseReal(s, end, StrtodAdapter); }
float StrToF(const char* s, char** end) { return ParseReal(s, end, StrtofAdapter); }

std::wstring FormatFloatW(double v)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%f", v);

    const char* dp = LocaleDecimalPoint();
    std::string text(buf);
    if (std::strcmp(dp, ".") != 0) {
        const size_t at = text.find(dp);
        if (at != std::string::npos) text.replace(at, std::strlen(dp), ".");
    }
    return std::wstring(text.begin(), text.end());
}

} // namespace vsfm
