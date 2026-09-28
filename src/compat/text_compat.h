/*
 *  VSFilterMod cross-platform port -- the handful of helpers that used to live
 *  in dsutil (text.h / DSUtil.cpp), plus portable stand-ins for the two Win32
 *  code-page conversion calls the SSA/SRT parsers need.
 */

#pragma once

#include "platform.h"
#include "cstring_compat.h"
#include "atlcoll_compat.h"

// ---------------------------------------------------------------------------
// Code-page conversion
// ---------------------------------------------------------------------------

/* Maps a Windows charset id (as stored in .ass style lines) to a code page. */
DWORD CharSetToCodePage(DWORD dwCharSet);

#if !defined(_WIN32)
/* Signature-compatible subsets of the Win32 originals: no flags are honoured,
 * cchWideChar/cbMultiByte of 0 means "measure", and -1 means "NUL terminated".
 * Backed by iconv. */
int MultiByteToWideChar(UINT codePage, DWORD flags, const char* mb, int cbMultiByte,
                        wchar_t* wide, int cchWideChar);
int WideCharToMultiByte(UINT codePage, DWORD flags, const wchar_t* wide, int cchWideChar,
                        char* mb, int cbMultiByte, const char* defaultChar, BOOL* usedDefault);
#endif

CStringW UTF8To16(LPCSTR utf8);
CStringA UTF16To8(LPCWSTR utf16);

/* Full path of the running executable, or an empty string if it cannot be
 * determined. Replaces GetModuleFileName(0, ...) in the "Update Details"
 * handler, which gates a script-supplied resource path on the host being
 * Aegisub. */
CStringW HostExecutablePath();

// ---------------------------------------------------------------------------
// Explode / Implode, unchanged in behaviour from dsutil/text.h
// ---------------------------------------------------------------------------

template <class T, typename SEP>
T Explode(T str, CAtlList<T>& sl, SEP sep, size_t limit = 0)
{
    sl.RemoveAll();

    for (ptrdiff_t i = 0, j = 0; ; i = j + 1) {
        j = str.Find(sep, (int)i);

        if (j < 0 || sl.GetCount() == limit - 1) {
            sl.AddTail(str.Mid((int)i).Trim());
            break;
        } else {
            sl.AddTail(str.Mid((int)i, (int)(j - i)).Trim());
        }
    }

    return sl.GetHead();
}

template <class T, typename SEP>
T ExplodeMin(T str, CAtlList<T>& sl, SEP sep, size_t limit = 0)
{
    Explode(str, sl, sep, limit);
    POSITION pos = sl.GetHeadPosition();
    while (pos) {
        POSITION tmp = pos;
        if (sl.GetNext(pos).IsEmpty())
            sl.RemoveAt(tmp);
    }
    if (sl.IsEmpty()) sl.AddTail(T());

    return sl.GetHead();
}

template <class T, typename SEP>
T Implode(CAtlList<T>& sl, SEP sep)
{
    T ret;
    POSITION pos = sl.GetHeadPosition();
    while (pos) {
        ret += sl.GetNext(pos);
        if (pos) ret += sep;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Deterministic rand()
//
// \rnd, \rndx/y/z/s and \jitter seed the C library's generator with srand()
// and draw from rand(). On Windows that is the Microsoft CRT's linear
// congruential generator -- in both msvcrt and the UCRT -- so upstream
// VSFilterMod output depends on exactly that sequence. glibc and the macOS
// libc use entirely different algorithms (and a different RAND_MAX), so the
// same script would jitter to different positions on every platform.
//
// These reproduce the Microsoft generator bit for bit, with per-thread state
// as the CRT has, so a seeded effect lands on the same pixels everywhere.
// ---------------------------------------------------------------------------

void vsfm_srand(unsigned int seed);
int  vsfm_rand();

#define VSFM_RAND_MAX 0x7FFF
