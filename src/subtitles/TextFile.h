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

#pragma once

#include "../compat/vsfm_compat.h"

#include <cstdio>
#include <vector>

/* Byte-level stream, replacing MFC's CStdioFile. Two backends:
 *
 *   - a FILE*, always opened in binary mode (the subtitle format probes seek
 *     by byte offset, which text mode cannot deliver),
 *   - a caller-supplied memory block.
 *
 * The memory backend exists so CSimpleTextSubtitle::Open(BYTE*, int, ...) can
 * parse in place. It used to spool the buffer to a temp file and reopen it,
 * which meant a file create + write + parse + unlink on every single call --
 * and CSRI hands us the whole script again on every edit while Aegisub is
 * open.
 */
class CByteStream
{
public:
    enum { begin = 0, current = 1, end = 2 };

    CByteStream();
    virtual ~CByteStream();

    bool OpenRead(LPCTSTR path);
    bool OpenMemory(const BYTE* data, size_t len);
    bool OpenWrite(LPCTSTR path);
    virtual void Close();

    bool IsOpen() const { return m_fp != nullptr || m_mem != nullptr; }

    size_t Read(void* buf, size_t count);
    size_t Write(const void* buf, size_t count);

    ULONGLONG RawPosition() const;
    ULONGLONG RawLength() const;
    ULONGLONG RawSeek(LONGLONG off, int from);

protected:
    CString m_strFileName;

private:
    CByteStream(const CByteStream&) = delete;
    CByteStream& operator=(const CByteStream&) = delete;

    FILE* m_fp;
    const BYTE* m_mem;
    size_t m_memLen;
    size_t m_memPos;
    bool m_writing;
};

class CTextFile : protected CByteStream
{
public:
    typedef enum {ASCII, UTF8, LE16, BE16, ANSI} enc;

private:
    enc m_encoding, m_defaultencoding;
    int m_offset;

    bool DetectEncoding();

public:
    CTextFile(enc e = ASCII);

    virtual bool Open(LPCTSTR lpszFileName);
    /* Parses a script already in memory. `name` is only used for diagnostics
     * and for GetFilePath(). */
    virtual bool OpenFromMemory(const BYTE* data, size_t len, LPCTSTR name);
    virtual bool Save(LPCTSTR lpszFileName, enc e /*= ASCII*/);

    using CByteStream::Close;

    void SetEncoding(enc e);
    enc GetEncoding();
    bool IsUnicode();

    CString GetFilePath() const;

    ULONGLONG GetPosition() const;
    ULONGLONG GetLength() const;
    ULONGLONG Seek(LONGLONG lOff, UINT nFrom);

    void WriteString(LPCSTR lpsz/*CStringA str*/);
    void WriteString(LPCWSTR lpsz/*CStringW str*/);
    BOOL ReadString(CStringA& str);
    BOOL ReadString(CStringW& str);
};
