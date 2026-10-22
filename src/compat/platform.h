/*
 *  VSFilterMod cross-platform port
 *
 *  Baseline platform types. On Windows we take them from <windows.h> so that
 *  the platform font backend (which must talk to DirectWrite/GDI) sees exactly
 *  one definition of POINT/RECT/SIZE. Everywhere else we define them ourselves.
 *
 *  Nothing in this tree calls a GDI drawing function any more -- glyph outlines
 *  come from FreeType on all three platforms -- so the Windows build only needs
 *  the types and the handful of charset constants.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <climits>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tchar.h>

/* Came from the DirectShow headers, which this tree no longer includes. */
#ifndef _REFERENCE_TIME_DEFINED
#define _REFERENCE_TIME_DEFINED
typedef LONGLONG REFERENCE_TIME;
#endif

#else  // ---------------------------------------------------------------- POSIX

typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef unsigned int        UINT;
typedef uint32_t            DWORD;
typedef int32_t             LONG;
typedef int64_t             LONGLONG;
typedef uint64_t            ULONGLONG;
typedef int64_t             __int64;
typedef int                 BOOL;
typedef unsigned char       UCHAR;
typedef char                CHAR;
typedef void*               HANDLE;
typedef DWORD               COLORREF;

typedef char*               LPSTR;
typedef const char*         LPCSTR;
typedef wchar_t*            LPWSTR;
typedef const wchar_t*      LPCWSTR;
typedef void*               LPVOID;
typedef const void*         LPCVOID;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#ifndef MAXLONG
#define MAXLONG  0x7fffffff
#endif

/* Locale id. Carried around in CSimpleTextSubtitle::m_lcid, which only the
 * DirectShow stream-info path ever populated; kept so the field still has a
 * type. */
typedef DWORD LCID;

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#ifndef _MAX_PATH
#define _MAX_PATH MAX_PATH
#endif

typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagSIZE  { LONG cx, cy; } SIZE, *LPSIZE;
typedef struct tagRECT  { LONG left, top, right, bottom; } RECT, *LPRECT;

/* GDI path point types, reproduced so Rasterizer's path walker is unchanged. */
#define PT_CLOSEFIGURE  0x01
#define PT_LINETO       0x02
#define PT_BEZIERTO     0x04
#define PT_MOVETO       0x06

/* Charset ids. Values are the Windows ones because .ass files store them. */
#define ANSI_CHARSET         0
#define DEFAULT_CHARSET      1
#define SYMBOL_CHARSET       2
#define MAC_CHARSET          77
#define SHIFTJIS_CHARSET     128
#define HANGUL_CHARSET       129
#define JOHAB_CHARSET        130
#define GB2312_CHARSET       134
#define CHINESEBIG5_CHARSET  136
#define GREEK_CHARSET        161
#define TURKISH_CHARSET      162
#define VIETNAMESE_CHARSET   163
#define HEBREW_CHARSET       177
#define ARABIC_CHARSET       178
#define BALTIC_CHARSET       186
#define RUSSIAN_CHARSET      204
#define THAI_CHARSET         222
#define EASTEUROPE_CHARSET   238
#define OEM_CHARSET          255

#define FW_DONTCARE   0
#define FW_THIN       100
#define FW_EXTRALIGHT 200
#define FW_LIGHT      300
#define FW_NORMAL     400
#define FW_MEDIUM     500
#define FW_SEMIBOLD   600
#define FW_BOLD       700
#define FW_EXTRABOLD  800
#define FW_HEAVY      900

#define CP_ACP    0
#define CP_UTF8   65001
#define CP_UTF7   65000

#define LF_FACESIZE 32

typedef struct tagLOGFONTW
{
    LONG    lfHeight;
    LONG    lfWidth;
    LONG    lfEscapement;
    LONG    lfOrientation;
    LONG    lfWeight;
    BYTE    lfItalic;
    BYTE    lfUnderline;
    BYTE    lfStrikeOut;
    BYTE    lfCharSet;
    BYTE    lfOutPrecision;
    BYTE    lfClipPrecision;
    BYTE    lfQuality;
    BYTE    lfPitchAndFamily;
    wchar_t lfFaceName[LF_FACESIZE];
} LOGFONTW;

typedef struct tagLOGFONTA
{
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    char lfFaceName[LF_FACESIZE];
} LOGFONTA;

typedef LOGFONTW LOGFONT;   // the tree is Unicode-only, as on Windows

/* Only referenced when building the LOGFONT; the FreeType backend ignores them. */
#define OUT_DEFAULT_PRECIS   0
#define OUT_TT_PRECIS        4
#define CLIP_DEFAULT_PRECIS  0
#define DEFAULT_QUALITY      0
#define ANTIALIASED_QUALITY  4
#define DEFAULT_PITCH        0
#define FF_DONTCARE          0

#endif  // _WIN32

/* ------------------------------------------------------------------------- */
/* Types and spellings the VSFilter sources use regardless of platform.       */
/* ------------------------------------------------------------------------- */

#if !defined(_WIN32)
typedef LONGLONG REFERENCE_TIME;
typedef wchar_t  TCHAR;
typedef wchar_t  WCHAR;
typedef TCHAR*   LPTSTR;
typedef const TCHAR* LPCTSTR;
#define _T(x)  L##x
#define TEXT(x) L##x
#endif

/* VSFilter allocates with DNew so leak-tracking builds could hook it. */
#ifndef DNew
#define DNew new
#endif

#ifndef ASSERT
#include <cassert>
#define ASSERT(x) assert(x)
#endif

#ifndef VERIFY
#define VERIFY(x) ((void)(x))
#endif

#ifndef TRACE
#define TRACE(...) ((void)0)
#endif

#if defined(_WIN32)
#define VSFM_PATH_SEPARATOR _T('\\')
#else
#define VSFM_PATH_SEPARATOR _T('/')
#endif

/* Came from dsutil/SharedInclude.h; used by the \move and rotation maths. */
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

/* MulDiv is used by the style -> LOGFONT conversion. */
#if !defined(_WIN32)
inline int MulDiv(int a, int b, int c)
{
    if (c == 0) return -1;
    int64_t r = (int64_t)a * b;
    r += (r < 0) ? -(c / 2) : (c / 2);
    return (int)(r / c);
}
#endif

/* Case-insensitive comparisons.
 *
 * The narrow ones map onto POSIX. The wide ones are implemented here rather
 * than mapped onto wcscasecmp/wcsncasecmp: those are POSIX.1-2008, but glibc
 * only declares them when _GNU_SOURCE or _POSIX_C_SOURCE >= 200809L is set and
 * macOS wants _DARWIN_C_SOURCE, so relying on them makes the build depend on
 * feature-test macros the host may not define. */
#if !defined(_WIN32)

#include <strings.h>
#define _stricmp  strcasecmp
#define _strnicmp strncasecmp

inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const wint_t ca = towlower((wint_t)a[i]);
        const wint_t cb = towlower((wint_t)b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
    }
    return 0;
}

inline int _wcsicmp(const wchar_t* a, const wchar_t* b)
{
    for (;; ++a, ++b) {
        const wint_t ca = towlower((wint_t)*a);
        const wint_t cb = towlower((wint_t)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
    }
}

#endif
