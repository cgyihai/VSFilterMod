/*
 *  VSFilterMod cross-platform port -- number parsing that does not depend on
 *  the platform or the host's locale.
 *
 *  The ASS parser reads every number with the C library's wcstol, wcstod and
 *  swscanf. Two things made the result differ between platforms:
 *
 *  - long is 32 bits on Windows and 64 bits on Linux and macOS. wcstol clamps
 *    out-of-range input to LONG_MAX, so \c&H80FF00FF& (alpha written into a
 *    colour tag, as some scripts do) parses as 0x7FFFFFFF -- white -- on
 *    Windows, which is what upstream draws, but as 0x80FF00FF -- magenta --
 *    elsewhere. WcsToL clamps to 32 bits on every platform.
 *
 *  - wcstod, strtod and scanf's %f take their decimal point from the C
 *    locale. A host that calls setlocale(LC_ALL, "") -- GUI toolkits do --
 *    under a German, French or Russian locale makes "\blur1.5" parse as 1.
 *    The functions below always use '.', as the ASS format does.
 */

#pragma once

#include <string>

namespace vsfm {

/* wcstol with Windows' 32-bit long: out-of-range values become
 * INT32_MAX / INT32_MIN. */
long WcsToL(const wchar_t* s, wchar_t** end, int base);

/* wcstod / wcstof / strtod / strtof that always read '.' as the decimal
 * point, whatever the current locale. */
double WcsToD(const wchar_t* s, wchar_t** end);
float WcsToF(const wchar_t* s, wchar_t** end);
double StrToD(const char* s, char** end);
float StrToF(const char* s, char** end);

/* printf's "%f", always with '.' as the decimal point. */
std::wstring FormatFloatW(double v);

} // namespace vsfm
