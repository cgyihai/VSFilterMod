/*
 *  VSFilterMod cross-platform port -- TCHAR-era helpers.
 *
 *  The tree was always built with _UNICODE, so TCHAR == WCHAR and the
 *  T <-> W conversions are identity. Only the A <-> W ones do real work.
 */

#pragma once

#include "platform.h"
#include "cstring_compat.h"

#ifndef countof
#define countof(array) (sizeof(array) / sizeof(array[0]))
#endif

/* Identity in a Unicode build. Kept as functions rather than macros so the
 * call sites read the same as before. */
/* By value, not by const reference: call sites chain non-const CString
 * methods onto the result, e.g. WToT(x).TrimLeft('#'). */
inline CStringW WToT(const CStringW& s) { return s; }
inline CStringW TToW(const CStringW& s) { return s; }

inline CStringA WToA(const CStringW& s) { return CStringA(s); }
inline CStringW AToW(const CStringA& s) { return CStringW(s); }
inline CStringA TToA(const CStringW& s) { return CStringA(s); }
inline CStringW AToT(const CStringA& s) { return CStringW(s); }

#if !defined(_WIN32)

/* platform.h's own _wcsnicmp/_wcsicmp, not wcsncasecmp/wcscasecmp: those
 * need _GNU_SOURCE or _DARWIN_C_SOURCE to be declared at all. */
#define _tcsnicmp  _wcsnicmp
#define _tcsicmp   _wcsicmp
#define wcsnicmp   _wcsnicmp
#define wcsicmp    _wcsicmp
#define _istspace  iswspace
#define _istdigit  iswdigit
#define _istupper  iswupper
#define _istlower  iswlower
#define _istalpha  iswalpha
#define _istalnum  iswalnum
#define _istxdigit iswxdigit
#define _totupper  towupper
#define _totlower  towlower
#define _tcscmp    wcscmp
#define _tcsncmp   wcsncmp
#define _tcslen    wcslen
#define _tcscpy    wcscpy
#define _tcschr    wcschr
#define _tcsrchr   wcsrchr
#define _tcsstr    wcsstr
#define _tcstol    wcstol
#define _tcstod    wcstod
#define _ttoi(s)   ((int)wcstol((s), nullptr, 10))
#define _ttof(s)   wcstod((s), nullptr)
#define _stscanf   swscanf
#define _snwprintf swprintf

#endif // !_WIN32

/* Bounded copy into a fixed-size buffer, always NUL terminating.
 *
 * The LOGFONT conversions used MSVC's strncpy_s/wcsncpy_s. Defining those
 * names ourselves on POSIX is a trap: any libc that implements Annex K
 * declares them with a different signature, and the build then fails on a
 * redeclaration rather than on anything meaningful. A distinct name sidesteps
 * the collision on every platform. */
inline void VsfmCopyFixed(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0) return;
    std::strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

inline void VsfmCopyFixed(wchar_t* dst, size_t cap, const wchar_t* src)
{
    if (!dst || cap == 0) return;
    std::wcsncpy(dst, src ? src : L"", cap - 1);
    dst[cap - 1] = L'\0';
}

/* Screen DPI used to be read from a live HDC via GetDeviceCaps(LOGPIXELSY).
 * It is pinned here instead: .ass point sizes are defined against 96 DPI, and
 * letting the host's display scaling leak in would make the same script render
 * at different sizes on different machines -- and make BDN+XML output depend
 * on the monitor it was generated on. */
#ifndef VSFM_LOGPIXELSY
#define VSFM_LOGPIXELSY 96
#endif

/* Minimal stand-in for ATL's scoped array holder. */
template <typename T>
class CAutoVectorPtr
{
public:
    CAutoVectorPtr() : m_p(nullptr) {}
    ~CAutoVectorPtr() { delete[] m_p; }
    CAutoVectorPtr(const CAutoVectorPtr&) = delete;
    CAutoVectorPtr& operator=(const CAutoVectorPtr&) = delete;

    bool Allocate(size_t n)
    {
        delete[] m_p;
        m_p = new (std::nothrow) T[n];
        return m_p != nullptr;
    }
    void Free() { delete[] m_p; m_p = nullptr; }
    T* Detach() { T* p = m_p; m_p = nullptr; return p; }

    /* No operator[]: the implicit T* conversion already indexes, and
     * declaring both makes every pData[i] ambiguous. */
    operator T*() const { return m_p; }

    T* m_p;
};

template <typename T>
class CAutoPtr
{
public:
    CAutoPtr() : m_p(nullptr) {}
    explicit CAutoPtr(T* p) : m_p(p) {}
    ~CAutoPtr() { delete m_p; }
    CAutoPtr(const CAutoPtr&) = delete;
    CAutoPtr& operator=(const CAutoPtr&) = delete;

    void Attach(T* p) { delete m_p; m_p = p; }
    T* Detach() { T* p = m_p; m_p = nullptr; return p; }
    void Free() { delete m_p; m_p = nullptr; }

    T* operator->() const { return m_p; }
    operator T*() const { return m_p; }

    T* m_p;
};
