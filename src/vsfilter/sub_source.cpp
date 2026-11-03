/*
 *  VSFilterMod cross-platform port -- the render path shared by the AviSynth+
 *  and VapourSynth entry points. See sub_source.h.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#include "sub_source.h"

#include <cstring>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace vsfm {

namespace {

/* The subpicture queue's frame rate, which upstream's frame-server filters
 * never set. RTS only uses it for frame-timed formats (MicroDVD). */
const double kQueueFps = 25.0;

#if defined(_WIN32)
bool FileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Convert(const char* s, UINT cp)
{
    const int n = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s, -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
#endif

} // namespace

std::wstring ScriptPath(const char* s, bool preferUtf8)
{
    if (!s) return std::wstring();
#if defined(_WIN32)
    const std::wstring first = Convert(s, preferUtf8 ? CP_UTF8 : CP_ACP);
    if (FileExists(first)) return first;
    const std::wstring second = Convert(s, preferUtf8 ? CP_ACP : CP_UTF8);
    return FileExists(second) ? second : first;
#else
    (void)preferUtf8;
    return U82W(s, std::strlen(s));
#endif
}

SubtitleSource::SubtitleSource() = default;
SubtitleSource::~SubtitleSource() = default;

/* Nothing below lets a C++ exception escape: both callers are entered from
 * a frame server through a C ABI. A script that makes the parser or the
 * renderer throw fails to open, or draws nothing on that frame. */

bool SubtitleSource::Open(const std::wstring& path, int charset)
{
    try {
        m_rts.reset(new CRenderedTextSubtitle(&m_rtsLock));
        if (m_rts->Open(CString(path.c_str()), charset)) {
            m_matrix = m_rts->m_eYCbCrMatrix;
            m_range = m_rts->m_eYCbCrRange;
            return true;
        }
    } catch (...) {
    }
    m_rts.reset();
    return false;
}

bool SubtitleSource::OpenVfr(const std::wstring& path)
{
    try {
        m_vfr = GetVFRTranslator(path);
    } catch (...) {
        m_vfr.reset();
    }
    return (bool)m_vfr;
}

REFERENCE_TIME SubtitleSource::Timestamp(int n, float fps) const
{
    if (m_vfr)
        return (REFERENCE_TIME)(10000000 * m_vfr->TimeStampFromFrameNumber(n));

    /* 10000000i64 * n / fps: the int64 operand is converted to float and the
     * division is single precision. volatile keeps a compiler from carrying
     * either in a wider register. */
    volatile float num = (float)(10000000LL * (long long)n);
    volatile float q = num / fps;
    return (REFERENCE_TIME)q;
}

const MemSubPic* SubtitleSource::Render(REFERENCE_TIME rt, int type, int w, int h)
{
    try {
        return RenderUnguarded(rt, type, w, h);
    } catch (...) {
        /* The subpicture may hold a partial render with no dirty rectangle
         * recorded to clear it by; start the next frame from a fresh one. */
        m_subpic.reset();
        m_cachedStart = m_cachedStop = 0;
        return nullptr;
    }
}

const MemSubPic* SubtitleSource::RenderUnguarded(REFERENCE_TIME rt, int type, int w, int h)
{
    if (!m_rts) return nullptr;

    // CMemSubPicAllocator: one subpicture, typed after the first frame.
    if (!m_subpic)
        m_subpic.reset(new MemSubPic(w, h, type, m_matrix, m_range));

    // CSubPicQueueNoThread::LookupSubPic
    if (m_cachedStart <= rt && rt < m_cachedStop) return m_subpic.get();

    CAutoLock rtsGuard(&m_rtsLock);
    POSITION pos = m_rts->GetStartPosition(rt, kQueueFps);
    if (!pos) return nullptr;

    // IsAnimated() is always true, so the interval is [rt, rt + 1).
    const REFERENCE_TIME start = rt, stop = rt + 1;
    SubPicDesc& spd = m_subpic->Lock();
    CRect r(0, 0, 0, 0);
    m_rts->Render(spd, (start + stop) / 2, kQueueFps, r);
    m_subpic->Unlock(r);
    m_cachedStart = start;
    m_cachedStop = stop;
    return m_subpic.get();
}

} // namespace vsfm
