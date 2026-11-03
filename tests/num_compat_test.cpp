/*
 * Checks that number parsing gives the same result on every platform and
 * under every locale (see src/compat/num_compat.h).
 *
 * The locale part needs a locale whose decimal point is not '.'; when none
 * is installed it is reported as skipped. On Linux one can be generated
 * without root:  localedef -i de_DE -f UTF-8 DIR/de_DE.UTF-8  and run the
 * test with LOCPATH=DIR.
 */

#include "compat/num_compat.h"

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void CheckParsing(const char* label)
{
    wchar_t* e = nullptr;

    const wchar_t* blur = L"1.5\\bord2";
    Check(vsfm::WcsToD(blur, &e) == 1.5, "WcsToD reads 1.5");
    Check(e == blur + 3, "WcsToD stops after 1.5");

    const wchar_t* exp = L"  -2.5e2}";
    Check(vsfm::WcsToD(exp, &e) == -250.0 && e == exp + 8, "WcsToD reads -2.5e2");

    const wchar_t* comma = L"1,5";
    Check(vsfm::WcsToD(comma, &e) == 1.0 && e == comma + 1, "WcsToD stops at a comma");

    const wchar_t* none = L"  x";
    Check(vsfm::WcsToD(none, &e) == 0.0 && e == none, "WcsToD with no number leaves end at s");

    Check(vsfm::WcsToF(L"0.1", nullptr) == 0.1f, "WcsToF rounds once to float");
    Check(vsfm::StrToD("23.976", nullptr) == 23.976, "StrToD reads 23.976");
    Check(vsfm::StrToF("29.97,", nullptr) == 29.97f, "StrToF reads 29.97");

    const std::wstring f = vsfm::FormatFloatW(12.5);
    Check(f == L"12.500000", "FormatFloatW writes 12.500000");

    std::printf("parsing under %s: done\n", label);
}

} // namespace

int main()
{
    // 32-bit long semantics, as on Windows.
    Check(vsfm::WcsToL(L"80FF00FF", nullptr, 16) == INT32_MAX, "WcsToL clamps 0x80FF00FF");
    Check(vsfm::WcsToL(L"FFFFFFFF", nullptr, 16) == INT32_MAX, "WcsToL clamps 0xFFFFFFFF");
    Check(vsfm::WcsToL(L"-99999999999", nullptr, 10) == INT32_MIN, "WcsToL clamps below");
    Check(vsfm::WcsToL(L"00FF00FF", nullptr, 16) == 0x00FF00FF, "WcsToL in range");
    Check(vsfm::WcsToL(L"-12px", nullptr, 10) == -12, "WcsToL negative");
    Check((uint32_t)vsfm::WcsToL(L"-1", nullptr, 16) == 0xFFFFFFFFu, "WcsToL -1 as DWORD");

    CheckParsing("the C locale");

    const char* commaLocales[] = {
        "de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8",
        "ru_RU.UTF-8", "German_Germany.1252", "de-DE", nullptr,
    };
    const char* used = nullptr;
    for (const char** l = commaLocales; *l; ++l) {
        if (std::setlocale(LC_ALL, *l)) {
            const struct lconv* lc = std::localeconv();
            if (lc && std::strcmp(lc->decimal_point, ".") != 0) { used = *l; break; }
        }
    }
    if (used) {
        CheckParsing(used);
        std::setlocale(LC_ALL, "C");
    } else {
        std::printf("no locale with a ',' decimal point installed: locale check skipped\n");
    }

    std::printf(g_failures ? "FAILED\n" : "ok\n");
    return g_failures ? 1 : 0;
}
