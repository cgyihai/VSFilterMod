/*
 *  Compiles the POSIX branch of the compatibility layer on a Windows host.
 *
 *  Every system header the layer needs is pulled in first, while _WIN32 is
 *  still defined and the platform's own headers are therefore happy. _WIN32 is
 *  then undefined, so our headers take their POSIX path: the hand-written
 *  BYTE/DWORD/POINT/RECT/LOGFONTW definitions, the TCHAR mappings, HRESULT,
 *  and the POSIX spellings of the case-insensitive compare functions.
 *
 *  This does not replace a real Linux or macOS build -- it cannot check
 *  iconv, fontconfig, CoreText, dlopen, or the 32-bit wchar_t assumption --
 *  but it does catch typos and missing declarations in code paths that a
 *  Windows build never instantiates.
 *
 *  Compiled, never linked: the objects use the POSIX manglings and so
 *  cannot link against the rest of a Windows build.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <climits>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <list>
#include <memory>
#include <mutex>
#include <algorithm>
#include <utility>
#include <type_traits>
#include <fstream>

#include <png.h>

/* From here on, pretend we are not on Windows. */
#undef _WIN32
#undef _WIN64
#undef WIN32
#undef __MINGW32__

/* MinGW makes __int64 a keyword, so `typedef int64_t __int64;` in the POSIX
 * branch of platform.h declares nothing there. GCC and Clang on Linux and
 * macOS have no such keyword, so the typedef is what those builds need.
 * Rename it out of the way for the probe only. */
#undef __int64
#define __int64 vsfm_probe_int64

/* MinGW's wchar_t is 16 bits; the POSIX build assumes 32. Nothing in the
 * headers depends on the width, so the probe simply notes the difference. */

#include "../src/compat/vsfm_compat.h"
#include "../src/compat/simd_compat.h"
#include "../src/compat/cpu_compat.h"
#include "../src/subtitles/STS.h"
#include "../src/subtitles/Rasterizer.h"
#include "../src/subtitles/RTS.h"
#include "../src/font/font_engine.h"
#include "../src/font/font_provider.h"

/* Force instantiation of the templates the renderer actually uses, so the
 * probe fails on a POSIX-only typo inside them rather than silently skipping. */
void instantiate()
{
    CStringW w = L"abc";
    w.Format(_T("%s %d %f"), (LPCWSTR)w, 1, 2.0);
    w.Trim();
    w.MakeLower();
    w.Replace(L"a", L"b");
    w += L'x';
    (void)(w == L"abc");
    (void)(w == "abc");
    (void)(w != '0');
    (void)w.Mid(1).Left(1).Find(L'a');
    int tokPos = 0;
    (void)w.Tokenize(L",", tokPos);

    CStringA a(w);
    CStringW back(a);
    (void)back;

    CAtlArray<int> arr;
    arr.Add(1);
    arr.SetAt(4, 2);
    (void)arr.GetCount();
    (void)arr.GetData();

    CAtlList<CStringW> list;
    Explode(CStringW(L"a,b,c"), list, ',');
    (void)Implode(list, ',');

    CAtlMap<CStringW, int> map;
    map.SetAt(L"k", 1);
    int v = 0;
    (void)map.Lookup(L"k", v);
    POSITION pos = map.GetStartPosition();
    while (pos) { CStringW k; int val; map.GetNextAssoc(pos, k, val); }

    CRect r(0, 0, 10, 10);
    CRect r2 = r + CRect(1, 1, 1, 1);
    (void)r2.Width();
    (void)(r & r2);
    (void)r.CenterPoint();

    CCritSec cs;
    { CAutoLock lock(&cs); }

    (void)CharSetToCodePage(SHIFTJIS_CHARSET);
    (void)UTF8To16("x");
    (void)UTF16To8(L"x");
    (void)HostExecutablePath();
    (void)MulDiv(3, 4, 5);

    vsfm::FontDesc desc;
    desc.faceName = L"Arial";
    desc.height = 100;
    (void)(desc == desc);

    HRESULT hr = S_OK;
    (void)SUCCEEDED(hr);
    (void)FAILED(hr);

    SubPicDesc spd;
    spd.type = MSP_RGBA;
    (void)spd;
}

/* Referenced so the compiler cannot discard the body. */
void (*vsfm_posix_probe_keep)() = &instantiate;
