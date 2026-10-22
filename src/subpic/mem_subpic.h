/*
 *  VSFilterMod cross-platform port -- the in-memory subpicture.
 *
 *  Upstream's AviSynth filters never let RTS draw straight into the video
 *  frame. They render into a CMemSubPic -- a full-canvas 32-bit surface --
 *  convert it for the destination format on Unlock, and composite it with
 *  AlphaBlt. This is that object without the COM wrapper, kept so that the
 *  AviSynth entry points reproduce upstream's output exactly, including:
 *
 *    - only the rendered bounding box is ever composited (AlphaBlt(r, r)),
 *    - the RGBA path's un-premultiply formula used by MaskSubMod,
 *    - the YUV conversion tables, selected by the script's YCbCr Matrix.
 *
 *  Formats: RGB32, RGBA (MaskSubMod), RGB24, YUY2, YV12, IYUV -- the set
 *  upstream's AviSynth filters mapped AviSynth clips onto.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#pragma once

#include "../compat/vsfm_compat.h"

#include <vector>

namespace vsfm {

/* Same numbering as CSimpleTextSubtitle's YCbCrMatrix / YCbCrRange, so the
 * script's values can be passed straight through. AUTO means BT.601 / TV. */
enum { YCC_MATRIX_BT601 = 0, YCC_MATRIX_BT709 = 1, YCC_MATRIX_BT2020 = 2, YCC_MATRIX_AUTO = 3 };
enum { YCC_RANGE_PC = 0, YCC_RANGE_TV = 1, YCC_RANGE_AUTO = 2 };

class MemSubPic
{
public:
    /* `type` is the destination's MSP_* format; the surface is always 32-bit. */
    MemSubPic(int width, int height, int type, int matrix, int range);

    /* Clears what the previous frame dirtied to 0xFF000000 (transparent in the
     * rasterizer's inverted-alpha convention) and returns a descriptor for RTS
     * to draw into. The first call clears the whole canvas. */
    SubPicDesc& Lock();

    /* Records `dirty` (RTS's returned bbox) and converts that area in place for
     * a YUV destination, widening it to even coordinates as upstream did. */
    void Unlock(const RECT& dirty);

    /* Composites the dirty area onto `target` at the same position. */
    bool AlphaBlt(SubPicDesc& target) const;

    const RECT& Dirty() const { return m_rcDirty; }

private:
    std::vector<DWORD> m_pixels;
    SubPicDesc m_spd;
    CRect m_rcDirty;
    int m_matrix;
    int m_range;
};

bool IsSupportedSubPicType(int type);

} // namespace vsfm
