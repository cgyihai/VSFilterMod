/*
 *  VSFilterMod cross-platform port -- the render path shared by the AviSynth+
 *  and VapourSynth entry points.
 *
 *  Upstream's frame-server filters (CFilter / CTextSubFilter in plugins.cpp)
 *  all rendered the same way, and this is that path:
 *
 *    - timestamp: (REFERENCE_TIME)(10000000i64 * n / fps) with fps a float,
 *      so the division is single precision; or the VFR timecode;
 *    - CSubPicQueueNoThread: RTS's IsAnimated() is always true, so a frame
 *      renders at exactly its timestamp, into one in-memory subpicture that is
 *      cleared over its previous dirty rectangle, and is reused only for that
 *      same timestamp;
 *    - RTS is handed fps = 25.0, the queue's default, which upstream never
 *      changed from its frame-server filters.
 *
 *  Not thread-safe: the callers serialise Render() and the composite after
 *  it, as upstream's queue did.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#pragma once

#include "../compat/vsfm_compat.h"
#include "../subtitles/RTS.h"
#include "../subpic/mem_subpic.h"
#include "vfr.h"

#include <memory>
#include <string>

namespace vsfm {

/* A path handed over by a script. On Windows it may be in the ANSI code page
 * or in UTF-8, and upstream tried both: AviSynth scripts are ANSI by default,
 * VapourSynth's are UTF-8. The preferred reading is taken when it names an
 * existing file, else the other one when that does, else the preferred one.
 * Elsewhere scripts are UTF-8. */
std::wstring ScriptPath(const char* s, bool preferUtf8);

class SubtitleSource
{
public:
    SubtitleSource();
    ~SubtitleSource();

    bool Open(const std::wstring& path, int charset);
    bool OpenVfr(const std::wstring& path);
    bool HasVfr() const { return (bool)m_vfr; }

    /* Frame n's timestamp, as upstream computed it (fps is only used without
     * VFR timecodes). */
    REFERENCE_TIME Timestamp(int n, float fps) const;

    /* Renders the subtitle at `rt` for a w x h destination of MSP_* `type` and
     * returns the subpicture to composite, or nullptr when nothing is shown.
     * The subpicture is created on the first call, typed after it. */
    const MemSubPic* Render(REFERENCE_TIME rt, int type, int w, int h);

    int Matrix() const { return m_matrix; }
    int Range() const { return m_range; }

private:
    CCritSec m_rtsLock;
    std::unique_ptr<CRenderedTextSubtitle> m_rts;
    std::unique_ptr<MemSubPic> m_subpic;
    std::unique_ptr<VFRTranslator> m_vfr;
    int m_matrix = YCC_MATRIX_AUTO;
    int m_range = YCC_RANGE_AUTO;

    /* The queue's cached subpicture interval. */
    REFERENCE_TIME m_cachedStart = 0, m_cachedStop = 0;
};

} // namespace vsfm
