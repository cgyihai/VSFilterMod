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

    /* Composites the dirty area onto separate planes of 8 to 16 bits per
     * sample, top-down -- VapourSynth's layout. For PLANES_YUV and
     * PLANES_GRAY the subpicture must be of a YUV type (it holds converted
     * samples): YV12 or IYUV for 4:2:0, any of them -- YUY2 keeps odd heights
     * whole -- for 4:2:2 and gray. For PLANES_RGB it must be MSP_RGB32
     * (premultiplied B, G, R). As with every YUV type, the conversion works on
     * horizontal pairs, so the last column of an odd-width gray clip is
     * never drawn on.
     *
     * The arithmetic is AlphaBlt's, carried out at the target's precision:
     * the subtitle's 8-bit values are scaled to the target's bit depth, the
     * video is not reduced to 8 bits, and results are clamped. For 8-bit RGB
     * it is exactly AlphaBlt's MSP_RGB32 formula. */
    enum { PLANES_YUV, PLANES_GRAY, PLANES_RGB };
    struct Planes
    {
        int family = PLANES_YUV;
        int bits = 8;               // 8..16; samples are 16-bit above 8
        int subW = 1, subH = 1;     // chroma subsampling, log2: (1,1) 4:2:0, (1,0) 4:2:2
        unsigned char* data[3] = {};
        ptrdiff_t stride[3] = {};   // bytes
    };
    bool AlphaBltPlanes(const Planes& target) const;

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
