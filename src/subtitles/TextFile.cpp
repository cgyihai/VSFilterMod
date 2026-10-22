/*
 *	Copyright (C) 2003-2006 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "TextFile.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// CByteStream
// ---------------------------------------------------------------------------

CByteStream::CByteStream()
    : m_fp(nullptr), m_mem(nullptr), m_memLen(0), m_memPos(0), m_writing(false)
{
}

CByteStream::~CByteStream()
{
    CByteStream::Close();
}

namespace {

FILE* OpenBinary(LPCTSTR path, bool write)
{
    if (!path) return nullptr;
#if defined(_WIN32)
    return _wfopen(path, write ? L"wb" : L"rb");
#else
    const CStringA narrow(path);
    return std::fopen((LPCSTR)narrow, write ? "wb" : "rb");
#endif
}

} // namespace

bool CByteStream::OpenRead(LPCTSTR path)
{
    Close();
    m_fp = OpenBinary(path, false);
    if (!m_fp) return false;
    m_strFileName = path;
    m_writing = false;
    return true;
}

bool CByteStream::OpenWrite(LPCTSTR path)
{
    Close();
    m_fp = OpenBinary(path, true);
    if (!m_fp) return false;
    m_strFileName = path;
    m_writing = true;
    return true;
}

bool CByteStream::OpenMemory(const BYTE* data, size_t len)
{
    Close();
    if (!data) return false;
    m_mem = data;
    m_memLen = len;
    m_memPos = 0;
    m_writing = false;
    return true;
}

void CByteStream::Close()
{
    if (m_fp) { std::fclose(m_fp); m_fp = nullptr; }
    m_mem = nullptr;
    m_memLen = m_memPos = 0;
    m_writing = false;
}

size_t CByteStream::Read(void* buf, size_t count)
{
    if (!buf || count == 0) return 0;

    if (m_fp) return std::fread(buf, 1, count, m_fp);

    if (m_mem) {
        const size_t avail = m_memLen - m_memPos;
        const size_t n = count < avail ? count : avail;
        if (n) std::memcpy(buf, m_mem + m_memPos, n);
        m_memPos += n;
        return n;
    }
    return 0;
}

size_t CByteStream::Write(const void* buf, size_t count)
{
    if (!m_fp || !m_writing || !buf || count == 0) return 0;
    return std::fwrite(buf, 1, count, m_fp);
}

ULONGLONG CByteStream::RawPosition() const
{
    if (m_fp) {
        const long p = std::ftell(m_fp);
        return p < 0 ? 0 : (ULONGLONG)p;
    }
    return (ULONGLONG)m_memPos;
}

ULONGLONG CByteStream::RawLength() const
{
    if (m_fp) {
        const long cur = std::ftell(m_fp);
        if (cur < 0) return 0;
        std::fseek(m_fp, 0, SEEK_END);
        const long end = std::ftell(m_fp);
        std::fseek(m_fp, cur, SEEK_SET);
        return end < 0 ? 0 : (ULONGLONG)end;
    }
    return (ULONGLONG)m_memLen;
}

ULONGLONG CByteStream::RawSeek(LONGLONG off, int from)
{
    if (m_fp) {
        const int whence = (from == current) ? SEEK_CUR : (from == end) ? SEEK_END : SEEK_SET;
        std::fseek(m_fp, (long)off, whence);
        return RawPosition();
    }
    if (m_mem) {
        LONGLONG target = off;
        if (from == current) target = (LONGLONG)m_memPos + off;
        else if (from == end) target = (LONGLONG)m_memLen + off;

        if (target < 0) target = 0;
        if (target > (LONGLONG)m_memLen) target = (LONGLONG)m_memLen;
        m_memPos = (size_t)target;
        return (ULONGLONG)m_memPos;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// CTextFile
// ---------------------------------------------------------------------------

CTextFile::CTextFile(enc e)
{
    m_encoding = m_defaultencoding = e;
    m_offset = 0;
}

/* Sniffs a byte-order mark and leaves the stream positioned past it. */
bool CTextFile::DetectEncoding()
{
    m_encoding = m_defaultencoding;
    m_offset = 0;

    const ULONGLONG len = RawLength();
    RawSeek(0, begin);

    if (len >= 2) {
        BYTE bom[2];
        if (Read(bom, 2) != 2) { Close(); return false; }

        const WORD w = (WORD)(bom[0] | (bom[1] << 8));

        if (w == 0xfeff) {
            /* FF FE is either a UTF-16LE BOM or the start of a UTF-32LE one
             * (FF FE 00 00). UTF-32 is not supported; reject it rather than
             * decode the embedded NULs as characters. */
            if (len >= 4) {
                BYTE b[2];
                if (Read(b, 2) != 2) { Close(); return false; }
                if (b[0] == 0x00 && b[1] == 0x00) { Close(); return false; }
            }
            m_encoding = LE16;
            m_offset = 2;
        } else if (w == 0xfffe) {
            m_encoding = BE16;
            m_offset = 2;
        } else if (w == 0xbbef && len >= 3) {
            BYTE b;
            if (Read(&b, 1) == 1 && b == 0xbf) {
                m_encoding = UTF8;
                m_offset = 3;
            }
        }
    }

    RawSeek(m_offset, begin);
    return true;
}

bool CTextFile::Open(LPCTSTR lpszFileName)
{
    if (!OpenRead(lpszFileName)) return false;
    return DetectEncoding();
}

bool CTextFile::OpenFromMemory(const BYTE* data, size_t len, LPCTSTR name)
{
    if (!OpenMemory(data, len)) return false;
    m_strFileName = name ? name : _T("");
    return DetectEncoding();
}

bool CTextFile::Save(LPCTSTR lpszFileName, enc e)
{
    if (!OpenWrite(lpszFileName)) return false;

    if (e == UTF8) {
        BYTE b[3] = {0xef, 0xbb, 0xbf};
        Write(b, sizeof(b));
    } else if (e == LE16) {
        BYTE b[2] = {0xff, 0xfe};
        Write(b, sizeof(b));
    } else if (e == BE16) {
        BYTE b[2] = {0xfe, 0xff};
        Write(b, sizeof(b));
    }

    m_encoding = e;
    m_offset = 0;

    return true;
}

void CTextFile::SetEncoding(enc e)
{
    m_encoding = e;
}

CTextFile::enc CTextFile::GetEncoding()
{
    return m_encoding;
}

bool CTextFile::IsUnicode()
{
    return m_encoding == UTF8 || m_encoding == LE16 || m_encoding == BE16;
}

CString CTextFile::GetFilePath() const
{
    return m_strFileName;
}

ULONGLONG CTextFile::GetPosition() const
{
    const ULONGLONG p = RawPosition();
    return p >= (ULONGLONG)m_offset ? p - m_offset : 0;
}

ULONGLONG CTextFile::GetLength() const
{
    const ULONGLONG l = RawLength();
    return l >= (ULONGLONG)m_offset ? l - m_offset : 0;
}

ULONGLONG CTextFile::Seek(LONGLONG lOff, UINT nFrom)
{
    const ULONGLONG pos = GetPosition();
    const ULONGLONG len = GetLength();

    switch (nFrom) {
    default:
    case begin:   break;
    case current: lOff = (LONGLONG)pos + lOff; break;
    case end:     lOff = (LONGLONG)len - lOff; break;
    }

    if (lOff < 0) lOff = 0;
    if (lOff > (LONGLONG)len) lOff = (LONGLONG)len;
    lOff += m_offset;

    return RawSeek(lOff, begin) - m_offset;
}

// -- writing ----------------------------------------------------------------

namespace {

/* UTF-16 code units, written little- or big-endian explicitly. The original
 * wrote raw WCHARs, which is only correct where wchar_t happens to be 16 bits
 * and the host is little-endian. */
void WriteUtf16(CByteStream& s, uint32_t cp, bool bigEndian)
{
    uint16_t units[2];
    int count;

    if (cp >= 0x10000 && cp <= 0x10FFFF) {
        const uint32_t v = cp - 0x10000;
        units[0] = (uint16_t)(0xD800 + (v >> 10));
        units[1] = (uint16_t)(0xDC00 + (v & 0x3FF));
        count = 2;
    } else {
        units[0] = (uint16_t)cp;
        count = 1;
    }

    for (int i = 0; i < count; ++i) {
        BYTE b[2];
        if (bigEndian) { b[0] = (BYTE)(units[i] >> 8); b[1] = (BYTE)(units[i] & 0xff); }
        else           { b[0] = (BYTE)(units[i] & 0xff); b[1] = (BYTE)(units[i] >> 8); }
        s.Write(b, 2);
    }
}

void WriteUtf8(CByteStream& s, uint32_t cp)
{
    BYTE b[4];
    int n;

    if (cp < 0x80)          { b[0] = (BYTE)cp; n = 1; }
    else if (cp < 0x800)    { b[0] = (BYTE)(0xC0 | (cp >> 6));
                              b[1] = (BYTE)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000)  { b[0] = (BYTE)(0xE0 | (cp >> 12));
                              b[1] = (BYTE)(0x80 | ((cp >> 6) & 0x3F));
                              b[2] = (BYTE)(0x80 | (cp & 0x3F)); n = 3; }
    else                    { b[0] = (BYTE)(0xF0 | (cp >> 18));
                              b[1] = (BYTE)(0x80 | ((cp >> 12) & 0x3F));
                              b[2] = (BYTE)(0x80 | ((cp >> 6) & 0x3F));
                              b[3] = (BYTE)(0x80 | (cp & 0x3F)); n = 4; }

    s.Write(b, (size_t)n);
}

/* Walks a CStringW yielding code points, joining surrogate pairs where
 * wchar_t is 16 bits. */
uint32_t NextCodePoint(const CStringW& s, int& i)
{
    uint32_t cp = (uint32_t)(wchar_t)s.GetAt(i++);
    if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i < s.GetLength()) {
        const uint32_t lo = (uint32_t)(wchar_t)s.GetAt(i);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            ++i;
        }
    }
    return cp;
}

} // namespace

void CTextFile::WriteString(LPCSTR lpsz)
{
    CStringA str(lpsz);

    if (m_encoding == ASCII || m_encoding == ANSI) {
        str.Replace("\n", "\r\n");
        Write((LPCSTR)str, (size_t)str.GetLength());
    } else {
        WriteString((LPCWSTR)AToW(str));
    }
}

void CTextFile::WriteString(LPCWSTR lpsz)
{
    CStringW str(lpsz);

    if (m_encoding == ASCII || m_encoding == ANSI) {
        str.Replace(L"\n", L"\r\n");
        const CStringA stra(str);
        Write((LPCSTR)stra, (size_t)stra.GetLength());
        return;
    }

    str.Replace(L"\n", L"\r\n");

    int i = 0;
    while (i < str.GetLength()) {
        const uint32_t cp = NextCodePoint(str, i);
        if (m_encoding == UTF8)      WriteUtf8(*this, cp);
        else if (m_encoding == LE16) WriteUtf16(*this, cp, false);
        else if (m_encoding == BE16) WriteUtf16(*this, cp, true);
    }
}

// -- reading ----------------------------------------------------------------

namespace {

/* Reads one UTF-16 code unit as exactly two bytes. Reading sizeof(WCHAR)
 * bytes, as the original did, consumes four on any platform where wchar_t is
 * 32 bits and shreds the file. */
bool ReadUnit16(CByteStream& s, bool bigEndian, uint16_t* out)
{
    BYTE b[2];
    if (s.Read(b, 2) != 2) return false;
    *out = bigEndian ? (uint16_t)((b[0] << 8) | b[1])
                     : (uint16_t)((b[1] << 8) | b[0]);
    return true;
}

/* Appends a code point, splitting into surrogates where wchar_t is 16 bits. */
void AppendCodePoint(CStringW& str, uint32_t cp)
{
    if (cp > 0x10FFFF) cp = L'?';

    if (sizeof(wchar_t) >= 4 || cp < 0x10000) {
        str += (wchar_t)cp;
    } else {
        const uint32_t v = cp - 0x10000;
        str += (wchar_t)(0xD800 + (v >> 10));
        str += (wchar_t)(0xDC00 + (v & 0x3FF));
    }
}

/* Decodes one UTF-8 sequence from the stream. Returns false at EOF or on a
 * malformed lead byte. */
bool ReadUtf8CodePoint(CByteStream& s, uint32_t* out)
{
    BYTE b;
    if (s.Read(&b, 1) != 1) return false;

    int extra;
    uint32_t cp;

    if (!(b & 0x80))            { cp = b;        extra = 0; }
    else if ((b & 0xe0) == 0xc0) { cp = b & 0x1f; extra = 1; }
    else if ((b & 0xf0) == 0xe0) { cp = b & 0x0f; extra = 2; }
    else if ((b & 0xf8) == 0xf0) { cp = b & 0x07; extra = 3; }
    else { *out = L'?'; return true; }

    for (int k = 0; k < extra; ++k) {
        if (s.Read(&b, 1) != 1) return false;
        if ((b & 0xc0) != 0x80) { *out = L'?'; return true; }
        cp = (cp << 6) | (b & 0x3f);
    }

    *out = cp;
    return true;
}

} // namespace

BOOL CTextFile::ReadString(CStringA& str)
{
    bool fEOF = true;
    str.Empty();

    if (m_encoding == ASCII || m_encoding == ANSI) {
        char c;
        while (Read(&c, sizeof(c)) == sizeof(c)) {
            fEOF = false;
            if (c == '\r') continue;
            if (c == '\n') break;
            str += c;
        }
    } else if (m_encoding == UTF8) {
        uint32_t cp;
        while (ReadUtf8CodePoint(*this, &cp)) {
            fEOF = false;
            const char c = (cp < 0x80) ? (char)cp : '?';
            if (c == '\r') continue;
            if (c == '\n') break;
            str += c;
        }
    } else if (m_encoding == LE16 || m_encoding == BE16) {
        uint16_t w;
        while (ReadUnit16(*this, m_encoding == BE16, &w)) {
            fEOF = false;
            const char c = (w < 0x100) ? (char)w : '?';
            if (c == '\r') continue;
            if (c == '\n') break;
            str += c;
        }
    }

    return !fEOF;
}

BOOL CTextFile::ReadString(CStringW& str)
{
    bool fEOF = true;
    str.Empty();

    if (m_encoding == ASCII || m_encoding == ANSI) {
        CStringA stra;
        char c;
        while (Read(&c, sizeof(c)) == sizeof(c)) {
            fEOF = false;
            if (c == '\r') continue;
            if (c == '\n') break;
            stra += c;
        }
        str = CStringW(stra);
    } else if (m_encoding == UTF8) {
        uint32_t cp;
        while (ReadUtf8CodePoint(*this, &cp)) {
            fEOF = false;
            if (cp == '\r') continue;
            if (cp == '\n') break;
            AppendCodePoint(str, cp);
        }
    } else if (m_encoding == LE16 || m_encoding == BE16) {
        const bool be = (m_encoding == BE16);
        uint16_t w;
        while (ReadUnit16(*this, be, &w)) {
            fEOF = false;
            if (w == '\r') continue;
            if (w == '\n') break;

            uint32_t cp = w;
            if (w >= 0xD800 && w <= 0xDBFF) {
                /* Rejoin the pair so a 32-bit wchar_t gets one code point
                 * rather than two orphaned halves. */
                const ULONGLONG mark = RawPosition();
                uint16_t lo;
                if (ReadUnit16(*this, be, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000 + ((uint32_t)(w - 0xD800) << 10) + (lo - 0xDC00);
                else
                    RawSeek((LONGLONG)mark, begin);
            }
            AppendCodePoint(str, cp);
        }
    }

    return !fEOF;
}
