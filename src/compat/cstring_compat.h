/*
 *  VSFilterMod cross-platform port -- ATL/MFC CString replacement.
 *
 *  Only the surface actually used by RTS/STS/Rasterizer/TextFile is provided.
 *  Semantics follow ATL's CStringT, including the quirks the parser relies on:
 *    - Mid(i) with i past the end returns empty rather than throwing
 *    - Find returns -1 when not found
 *    - Trim() trims " \t\r\n\v\f"
 *    - Tokenize advances iStart past the delimiter run
 *
 *  Implicit CStringA <-> CStringW conversion goes through UTF-8 (ATL would use
 *  the active code page; UTF-8 is the portable choice and every call site in
 *  this tree either feeds UTF-8 or ASCII).
 */

#pragma once

#include "platform.h"

#include <string>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <cctype>

namespace vsfm {

std::string  W2U8(const wchar_t* s, size_t len);
std::wstring U82W(const char* s, size_t len);

/* Wide format strings are written in MSVC dialect ("%s" == wide). On POSIX the
 * wide printf family follows C99 ("%s" == narrow), so rewrite the specifiers. */
std::wstring NormalizeWideFormat(const wchar_t* fmt);
std::string  NormalizeNarrowFormat(const char* fmt);

// ---------------------------------------------------------------------------

template <typename T> struct CharTraitsOf;

template <> struct CharTraitsOf<char>
{
    typedef char CharType;
    typedef std::string StringType;
    static int  ToLower(int c) { return std::tolower((unsigned char)c); }
    static int  ToUpper(int c) { return std::toupper((unsigned char)c); }
    static bool IsSpace(int c) { return std::isspace((unsigned char)c) != 0; }
    static const char* DefaultTrimSet() { return " \t\r\n\v\f"; }
    static int FormatV(char* buf, size_t cap, const char* fmt, va_list ap)
    {
        return std::vsnprintf(buf, cap, fmt, ap);
    }
    static int CompareNoCase(const char* a, const char* b) { return _stricmp(a, b); }
};

template <> struct CharTraitsOf<wchar_t>
{
    typedef wchar_t CharType;
    typedef std::wstring StringType;
    static int  ToLower(int c) { return std::towlower((wint_t)c); }
    static int  ToUpper(int c) { return std::towupper((wint_t)c); }
    static bool IsSpace(int c) { return std::iswspace((wint_t)c) != 0; }
    static const wchar_t* DefaultTrimSet() { return L" \t\r\n\v\f"; }
    static int FormatV(wchar_t* buf, size_t cap, const wchar_t* fmt, va_list ap)
    {
        /* vswprintf returns -1 both on encoding errors and on truncation, so
         * callers must grow the buffer and retry rather than trust the value. */
#if defined(_WIN32)
        return _vsnwprintf(buf, cap, fmt, ap);
#else
        return std::vswprintf(buf, cap, fmt, ap);
#endif
    }
    static int CompareNoCase(const wchar_t* a, const wchar_t* b) { return _wcsicmp(a, b); }
};

// ---------------------------------------------------------------------------

template <typename T>
class CStringT
{
public:
    typedef T XCHAR;
    typedef typename std::conditional<std::is_same<T, char>::value, wchar_t, char>::type OtherT;
    typedef CharTraitsOf<T> Traits;
    typedef typename Traits::StringType StringType;

    CStringT() {}
    CStringT(const CStringT& o) : m_str(o.m_str) {}
    CStringT(CStringT&& o) noexcept : m_str(std::move(o.m_str)) {}
    CStringT(const T* p) { if (p) m_str = p; }
    CStringT(const T* p, int len) { if (p && len > 0) m_str.assign(p, (size_t)len); }
    CStringT(const StringType& s) : m_str(s) {}
    explicit CStringT(T ch, int rep = 1) { if (rep > 0) m_str.assign((size_t)rep, ch); }

    /* Cross-width construction / assignment (UTF-8 bridge). */
    CStringT(const CStringT<typename std::conditional<std::is_same<T, char>::value, wchar_t, char>::type>& o)
    {
        AssignOther(o.GetString(), (size_t)o.GetLength());
    }
    CStringT(const typename std::conditional<std::is_same<T, char>::value, wchar_t, char>::type* p)
    {
        if (p) AssignOther(p, OtherLen(p));
    }

    CStringT& operator=(const CStringT& o) { m_str = o.m_str; return *this; }
    CStringT& operator=(CStringT&& o) noexcept { m_str = std::move(o.m_str); return *this; }
    CStringT& operator=(const T* p) { m_str = p ? p : StringType(); return *this; }
    CStringT& operator=(T ch) { m_str.assign(1, ch); return *this; }

    // -- accessors ----------------------------------------------------------

    int GetLength() const { return (int)m_str.size(); }
    bool IsEmpty() const { return m_str.empty(); }
    void Empty() { m_str.clear(); }

    const T* GetString() const { return m_str.c_str(); }
    operator const T*() const { return m_str.c_str(); }

    T GetAt(int i) const { return (i >= 0 && i < (int)m_str.size()) ? m_str[(size_t)i] : (T)0; }
    void SetAt(int i, T ch) { if (i >= 0 && i < (int)m_str.size()) m_str[(size_t)i] = ch; }
    T operator[](int i) const { return GetAt(i); }

    /* ATL hands out a writable buffer of at least nLength chars. */
    T* GetBuffer(int nLength = -1)
    {
        if (nLength > (int)m_str.size()) m_str.resize((size_t)nLength, (T)0);
        else if (nLength < 0 && m_str.empty()) m_str.resize(1, (T)0);
        return &m_str[0];
    }
    T* GetBufferSetLength(int nLength)
    {
        m_str.resize(nLength > 0 ? (size_t)nLength : 0, (T)0);
        return m_str.empty() ? const_cast<T*>(m_str.c_str()) : &m_str[0];
    }
    void ReleaseBuffer(int nNewLength = -1)
    {
        if (nNewLength < 0) {
            size_t z = m_str.find((T)0);
            if (z != StringType::npos) m_str.resize(z);
        } else {
            m_str.resize((size_t)nNewLength);
        }
    }

    // -- search -------------------------------------------------------------

    int Find(T ch, int start = 0) const
    {
        if (start < 0) start = 0;
        if (start > (int)m_str.size()) return -1;
        size_t p = m_str.find(ch, (size_t)start);
        return p == StringType::npos ? -1 : (int)p;
    }
    int Find(const T* sub, int start = 0) const
    {
        if (!sub) return -1;
        if (start < 0) start = 0;
        if (start > (int)m_str.size()) return -1;
        size_t p = m_str.find(sub, (size_t)start);
        return p == StringType::npos ? -1 : (int)p;
    }
    int Find(const CStringT& sub, int start = 0) const { return Find(sub.GetString(), start); }

    int ReverseFind(T ch) const
    {
        size_t p = m_str.rfind(ch);
        return p == StringType::npos ? -1 : (int)p;
    }

    int FindOneOf(const T* set) const
    {
        size_t p = m_str.find_first_of(set);
        return p == StringType::npos ? -1 : (int)p;
    }

    // -- slicing ------------------------------------------------------------

    CStringT Mid(int start) const
    {
        if (start < 0) start = 0;
        if (start >= (int)m_str.size()) return CStringT();
        return CStringT(m_str.substr((size_t)start));
    }
    CStringT Mid(int start, int count) const
    {
        if (start < 0) start = 0;
        if (count < 0) count = 0;
        if (start >= (int)m_str.size()) return CStringT();
        return CStringT(m_str.substr((size_t)start, (size_t)count));
    }
    CStringT Left(int count) const
    {
        if (count <= 0) return CStringT();
        if (count >= (int)m_str.size()) return *this;
        return CStringT(m_str.substr(0, (size_t)count));
    }
    CStringT Right(int count) const
    {
        if (count <= 0) return CStringT();
        if (count >= (int)m_str.size()) return *this;
        return CStringT(m_str.substr(m_str.size() - (size_t)count));
    }

    CStringT SpanIncluding(const T* set) const
    {
        size_t p = m_str.find_first_not_of(set);
        return p == StringType::npos ? *this : CStringT(m_str.substr(0, p));
    }
    CStringT SpanExcluding(const T* set) const
    {
        size_t p = m_str.find_first_of(set);
        return p == StringType::npos ? *this : CStringT(m_str.substr(0, p));
    }

    /* ATL semantics: returns the next token, sets iStart past its terminator,
     * and sets iStart to -1 once the string is exhausted. */
    CStringT Tokenize(const T* delims, int& iStart) const
    {
        if (iStart < 0 || iStart > (int)m_str.size()) { iStart = -1; return CStringT(); }
        size_t b = m_str.find_first_not_of(delims, (size_t)iStart);
        if (b == StringType::npos) { iStart = -1; return CStringT(); }
        size_t e = m_str.find_first_of(delims, b);
        if (e == StringType::npos) { iStart = (int)m_str.size(); return CStringT(m_str.substr(b)); }
        iStart = (int)e + 1;
        return CStringT(m_str.substr(b, e - b));
    }

    // -- mutation -----------------------------------------------------------

    int Replace(T from, T to)
    {
        int n = 0;
        for (auto& c : m_str) if (c == from) { c = to; ++n; }
        return n;
    }
    int Replace(const T* from, const T* to)
    {
        if (!from || !*from) return 0;
        StringType f(from);
        StringType t;
        if (to) t = to;
        int n = 0;
        size_t p = 0;
        while ((p = m_str.find(f, p)) != StringType::npos) {
            m_str.replace(p, f.size(), t);
            p += t.size();
            ++n;
        }
        return n;
    }

    int Remove(T ch)
    {
        size_t before = m_str.size();
        m_str.erase(std::remove(m_str.begin(), m_str.end(), ch), m_str.end());
        return (int)(before - m_str.size());
    }

    int Delete(int index, int count = 1)
    {
        if (index < 0 || index >= (int)m_str.size() || count <= 0) return (int)m_str.size();
        m_str.erase((size_t)index, (size_t)count);
        return (int)m_str.size();
    }

    int Insert(int index, const T* p)
    {
        if (!p) return (int)m_str.size();
        if (index < 0) index = 0;
        if (index > (int)m_str.size()) index = (int)m_str.size();
        m_str.insert((size_t)index, p);
        return (int)m_str.size();
    }
    int Insert(int index, T ch)
    {
        T tmp[2] = { ch, (T)0 };
        return Insert(index, tmp);
    }

    void Append(const CStringT& o) { m_str += o.m_str; }
    void AppendChar(T ch) { m_str += ch; }

    CStringT& MakeLower()
    {
        for (auto& c : m_str) c = (T)Traits::ToLower((int)c);
        return *this;
    }
    CStringT& MakeUpper()
    {
        for (auto& c : m_str) c = (T)Traits::ToUpper((int)c);
        return *this;
    }
    CStringT& MakeReverse() { std::reverse(m_str.begin(), m_str.end()); return *this; }

    CStringT& TrimLeft() { return TrimLeft(Traits::DefaultTrimSet()); }
    CStringT& TrimLeft(T ch) { T s[2] = { ch, (T)0 }; return TrimLeft(s); }
    CStringT& TrimLeft(const T* set)
    {
        size_t p = m_str.find_first_not_of(set);
        m_str.erase(0, p == StringType::npos ? m_str.size() : p);
        return *this;
    }

    CStringT& TrimRight() { return TrimRight(Traits::DefaultTrimSet()); }
    CStringT& TrimRight(T ch) { T s[2] = { ch, (T)0 }; return TrimRight(s); }
    CStringT& TrimRight(const T* set)
    {
        size_t p = m_str.find_last_not_of(set);
        if (p == StringType::npos) m_str.clear();
        else m_str.erase(p + 1);
        return *this;
    }

    CStringT& Trim() { return TrimRight().TrimLeft(); }
    CStringT& Trim(T ch) { return TrimRight(ch).TrimLeft(ch); }
    CStringT& Trim(const T* set) { return TrimRight(set).TrimLeft(set); }

    // -- comparison ---------------------------------------------------------

    int Compare(const T* p) const { return m_str.compare(p ? p : StringType().c_str()); }
    int CompareNoCase(const T* p) const { return Traits::CompareNoCase(m_str.c_str(), p ? p : (const T*)StringType().c_str()); }

    // -- formatting ---------------------------------------------------------

    void Format(const T* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        FormatV(fmt, ap);
        va_end(ap);
    }
    void AppendFormat(const T* fmt, ...)
    {
        CStringT tmp;
        va_list ap;
        va_start(ap, fmt);
        tmp.FormatV(fmt, ap);
        va_end(ap);
        m_str += tmp.m_str;
    }
    void FormatV(const T* fmt, va_list ap)
    {
        StringType nf = NormalizeFormat(fmt);
        size_t cap = nf.size() + 256;
        for (int attempt = 0; attempt < 12; ++attempt) {
            std::vector<T> buf(cap);
            va_list copy;
            va_copy(copy, ap);
            int n = Traits::FormatV(&buf[0], cap, nf.c_str(), copy);
            va_end(copy);
            /* Narrow vsnprintf reports the needed length; wide vswprintf only
             * reports failure, so both paths just grow until it fits. */
            if (n >= 0 && (size_t)n < cap) { m_str.assign(&buf[0], (size_t)n); return; }
            cap *= 2;
        }
        m_str.clear();
    }

    // -- operators ----------------------------------------------------------

    CStringT& operator+=(const CStringT& o) { m_str += o.m_str; return *this; }
    CStringT& operator+=(const T* p) { if (p) m_str += p; return *this; }
    CStringT& operator+=(T ch) { m_str += ch; return *this; }

    /* Comparison and concatenation are hidden friends rather than free
     * function templates so that implicit conversions apply to the *other*
     * operand. ATL relies on this and so does the tag parser, which writes
     * things like `cmd == "1c"` (a narrow literal against a CStringW) and
     * `p != '0'` (a narrow character). Template argument deduction would
     * reject both. */
    friend bool operator==(const CStringT& a, const CStringT& b) { return a.m_str == b.m_str; }
    friend bool operator!=(const CStringT& a, const CStringT& b) { return a.m_str != b.m_str; }
    friend bool operator< (const CStringT& a, const CStringT& b) { return a.m_str <  b.m_str; }
    friend bool operator> (const CStringT& a, const CStringT& b) { return a.m_str >  b.m_str; }
    friend bool operator<=(const CStringT& a, const CStringT& b) { return a.m_str <= b.m_str; }
    friend bool operator>=(const CStringT& a, const CStringT& b) { return a.m_str >= b.m_str; }

    /* Overloads taking the raw pointer are required, not merely convenient:
     * CStringT converts implicitly to const T*, so without an exact match
     * `str == L"x"` is ambiguous between the built-in pointer comparison
     * (converting the left side) and the CStringT comparison (converting the
     * right side). ATL declares the same pair for the same reason. */
    friend bool operator==(const CStringT& a, const T* p) { return a.m_str == StringType(p ? p : (const T*)Empties()); }
    friend bool operator!=(const CStringT& a, const T* p) { return !(a == p); }
    friend bool operator==(const T* p, const CStringT& a) { return a == p; }
    friend bool operator!=(const T* p, const CStringT& a) { return !(a == p); }
    friend bool operator< (const CStringT& a, const T* p) { return a.m_str <  StringType(p ? p : (const T*)Empties()); }
    friend bool operator> (const CStringT& a, const T* p) { return a.m_str >  StringType(p ? p : (const T*)Empties()); }

    /* Narrow literal against a wide string (and vice versa); the tree does
     * this in a handful of tag comparisons. */
    friend bool operator==(const CStringT& a, const OtherT* p) { return a == CStringT(p); }
    friend bool operator!=(const CStringT& a, const OtherT* p) { return !(a == CStringT(p)); }
    friend bool operator==(const OtherT* p, const CStringT& a) { return a == CStringT(p); }
    friend bool operator!=(const OtherT* p, const CStringT& a) { return !(a == CStringT(p)); }

    friend bool operator==(const CStringT& a, T ch) { return a.m_str.size() == 1 && a.m_str[0] == ch; }
    friend bool operator!=(const CStringT& a, T ch) { return !(a == ch); }
    friend bool operator==(T ch, const CStringT& a) { return a == ch; }
    friend bool operator!=(T ch, const CStringT& a) { return !(a == ch); }

    friend CStringT operator+(const CStringT& a, const CStringT& b)
    { CStringT r(a); r += b; return r; }
    friend CStringT operator+(const CStringT& a, const T* p)
    { CStringT r(a); r += p; return r; }
    friend CStringT operator+(const T* p, const CStringT& b)
    { CStringT r(p); r += b; return r; }
    friend CStringT operator+(const CStringT& a, T ch)
    { CStringT r(a); r += ch; return r; }
    friend CStringT operator+(T ch, const CStringT& b)
    { CStringT r; r += ch; r += b; return r; }

    const StringType& str() const { return m_str; }

private:
    static const T* Empties() { static const T e[1] = { (T)0 }; return e; }

    static size_t OtherLen(const char* p) { return std::strlen(p); }
    static size_t OtherLen(const wchar_t* p) { return std::wcslen(p); }

    void AssignOther(const wchar_t* p, size_t len);  // -> char specialisation
    void AssignOther(const char* p, size_t len);     // -> wchar_t specialisation

    StringType NormalizeFormat(const T* fmt);

    StringType m_str;
};

template <> inline void CStringT<char>::AssignOther(const wchar_t* p, size_t len) { m_str = W2U8(p, len); }
template <> inline void CStringT<char>::AssignOther(const char*, size_t) {}
template <> inline void CStringT<wchar_t>::AssignOther(const char* p, size_t len) { m_str = U82W(p, len); }
template <> inline void CStringT<wchar_t>::AssignOther(const wchar_t*, size_t) {}

template <> inline std::string  CStringT<char>::NormalizeFormat(const char* f) { return NormalizeNarrowFormat(f); }
template <> inline std::wstring CStringT<wchar_t>::NormalizeFormat(const wchar_t* f) { return NormalizeWideFormat(f); }

} // namespace vsfm

typedef vsfm::CStringT<char>    CStringA;
typedef vsfm::CStringT<wchar_t> CStringW;
typedef CStringW                CString;   // the tree is Unicode-only
