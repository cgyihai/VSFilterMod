/*
 *  VSFilterMod cross-platform port -- the in-memory subpicture.
 *
 *  Ported from upstream's subpic/MemSubPic.cpp (Gabest). The arithmetic is
 *  kept line for line; the changes are:
 *
 *    - no COM, no allocator, no queue: one object per filter instance;
 *    - the 32-bit x86 inline-assembly branches are gone. Upstream's x64 build
 *      used the SSE2 intrinsics branch, which is kept on x86;
 *    - elsewhere the YUY2 blend uses a scalar version that saturates exactly
 *      like the SSE2 code (checked over every input). Upstream's own scalar fallback
 *      wrapped with `& 0xff` instead, so it disagreed with its SSE2 path on
 *      the pixels where the sum left 0..255;
 *    - RGB16/15, NV12, P010/P016 and AYUV are not carried over: no AviSynth
 *      clip was ever mapped onto them.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#include "mem_subpic.h"
#include "../compat/cpu_compat.h"
#include "../compat/simd_compat.h"

#include <mutex>

namespace vsfm {

namespace {

// ---------------------------------------------------------------------------
// Colour conversion tables (upstream: MemSubPic.cpp, ColorConvInitOther)
// ---------------------------------------------------------------------------

#define DEFINE_YUV_MATRIX(Kr, Kg, Kb) {                             \
    {   Kr             ,  Kg            ,   Kb             , 0 },   \
    {  -Kr / ((1-Kb)*2), -Kg / ((1-Kb)*2), (1-Kb) / ((1-Kb)*2), 0 },\
    { (1-Kr) / ((1-Kr)*2), -Kg / ((1-Kr)*2), -Kb / ((1-Kr)*2), 0 } \
}

#define DEFINE_YUV_MATRIX_INV(Kr, Kg, Kb) {         \
    { 1,  0                , 2*(1-Kr)          , 0 },\
    { 1, -2*(1-Kb)*Kb / Kg , -2*(1-Kr)*Kr / Kg , 0 },\
    { 1,  2*(1-Kb)         , 0                 , 0 } \
}

const float MATRIX_BT_601[3][4]      = DEFINE_YUV_MATRIX(0.299f, 0.587f, 0.114f);
const float MATRIX_BT_601_INV[3][4]  = DEFINE_YUV_MATRIX_INV(0.299f, 0.587f, 0.114f);
const float MATRIX_BT_709[3][4]      = DEFINE_YUV_MATRIX(0.2126f, 0.7152f, 0.0722f);
const float MATRIX_BT_709_INV[3][4]  = DEFINE_YUV_MATRIX_INV(0.2126f, 0.7152f, 0.0722f);
const float MATRIX_BT_2020[3][4]     = DEFINE_YUV_MATRIX(0.2627f, 0.678f, 0.0593f);
const float MATRIX_BT_2020_INV[3][4] = DEFINE_YUV_MATRIX_INV(0.2627f, 0.678f, 0.0593f);

const int YUV_PC[3][4] = {
    { 255,   0,   0,   0 },
    {   0, 255,   0, 128 },
    {   0,   0, 255, 128 }
};
const int YUV_TV[3][4] = {
    { 219,   0,   0,  16 },
    {   0, 224,   0, 128 },
    {   0,   0, 224, 128 }
};

unsigned char Clip_base[256 * 3];
unsigned char* const Clip = Clip_base + 256;

int c2y_cyb, c2y_cyg, c2y_cyr, c2y_cu, c2y_cv;
int y2c_cbu, y2c_cgu, y2c_cgv, y2c_crv;
int c2y_yb[256], c2y_yg[256], c2y_yr[256];
int y2c_bu[256], y2c_gu[256], y2c_gv[256], y2c_rv[256];
int cy_cy, cy_cy2;

const float (*MATRIX)[4] = MATRIX_BT_601;
const float (*MATRIX_INV)[4] = MATRIX_BT_601_INV;
const int (*RANGE)[4] = YUV_TV;

/* The tables are process-global, as upstream's were. Everything that reads
 * or rewrites them holds this lock for the whole conversion or blend. */
std::recursive_mutex s_colorConvLock;
int s_lastMatrix = -1;
int s_lastRange = -1;

void ColorConvInitOther(int matrix, int range)
{
    if (matrix == s_lastMatrix && range == s_lastRange) return;

    if (matrix == YCC_MATRIX_BT709) {
        MATRIX = MATRIX_BT_709;
        MATRIX_INV = MATRIX_BT_709_INV;
    } else if (matrix == YCC_MATRIX_BT2020) {
        MATRIX = MATRIX_BT_2020;
        MATRIX_INV = MATRIX_BT_2020_INV;
    } else {                                        // BT601, AUTO, unknown
        MATRIX = MATRIX_BT_601;
        MATRIX_INV = MATRIX_BT_601_INV;
    }
    RANGE = (range == YCC_RANGE_PC) ? YUV_PC : YUV_TV;   // TV, AUTO, unknown

    cy_cy  = int(255.0 / RANGE[0][0] * 65536 + 0.5);
    cy_cy2 = int(255.0 / RANGE[0][0] * 32768 + 0.5);
    c2y_cyb = int(MATRIX[0][2] * RANGE[0][0] / 255 * 65536 + 0.5);
    c2y_cyg = int(MATRIX[0][1] * RANGE[0][0] / 255 * 65536 + 0.5);
    c2y_cyr = int(MATRIX[0][0] * RANGE[0][0] / 255 * 65536 + 0.5);
    c2y_cu = int(1.0 / (MATRIX_INV[2][1] * 255 / RANGE[1][1]) * 1024 + 0.5);
    c2y_cv = int(1.0 / (MATRIX_INV[0][2] * 255 / RANGE[2][2]) * 1024 + 0.5);

    y2c_cbu = int((MATRIX_INV[2][1] * 255 / RANGE[1][1]) * 65536 + 0.5);
    y2c_cgu = int(MATRIX_INV[2][1] * 255 / RANGE[1][1] * (MATRIX[0][2] / MATRIX[0][1]) * 65536 + 0.5);
    y2c_cgv = int(MATRIX_INV[0][2] * 255 / RANGE[2][2] * (MATRIX[0][0] / MATRIX[0][1]) * 65536 + 0.5);
    y2c_crv = int((MATRIX_INV[0][2] * 255 / RANGE[2][2]) * 65536 + 0.5);

    for (int i = 0; i < 256; i++) {
        Clip_base[i] = 0;
        Clip_base[i + 256] = (unsigned char)i;
        Clip_base[i + 512] = 255;
    }
    for (int i = 0; i < 256; i++) {
        c2y_yb[i] = c2y_cyb * i;
        c2y_yg[i] = c2y_cyg * i;
        c2y_yr[i] = c2y_cyr * i;
        y2c_bu[i] = y2c_cbu * (i - 128);
        y2c_gu[i] = y2c_cgu * (i - 128);
        y2c_gv[i] = y2c_cgv * (i - 128);
        y2c_rv[i] = y2c_crv * (i - 128);
    }

    s_lastMatrix = matrix;
    s_lastRange = range;
}

bool IsYuv(int type) { return type == MSP_YUY2 || type == MSP_YV12 || type == MSP_IYUV; }
bool IsPlanar(int type) { return type == MSP_YV12 || type == MSP_IYUV; }

/* One YUY2 macropixel blend, bit-exact with upstream's SSE2 code: signed
 * 16-bit (d - bias) * (a >> 1), arithmetic >> 7, saturating add of the
 * subtitle's premultiplied value, unsigned saturation back to a byte. */
inline BYTE BlendYuy2Channel(int d, int bias, int alpha, int c)
{
    int v = ((int)(short)((d - bias) * (alpha >> 1))) >> 7;
    v += c;
    return (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* (v >> 8) rounded towards minus infinity, without relying on the
 * implementation-defined right shift of a negative int. */
inline int Sar8(int v) { return v >= 0 ? (v >> 8) : ~(~v >> 8); }

inline int ClampSample(int v, int max) { return v < 0 ? 0 : (v > max ? max : v); }

/* An 8-bit full-range value at `bits` bits: 0 -> 0, 255 -> max. */
inline int ScaleFull(int v, int sh) { return (v << sh) | (v >> (8 - sh)); }

template <typename T>
inline T* Row(unsigned char* base, ptrdiff_t stride, int y)
{
    return (T*)(base + stride * y);
}

template <typename T>
void BlendYuvPlanes(const MemSubPic::Planes& t, const BYTE* src, int pitch,
                    const CRect& r, bool chroma)
{
    const int sh = t.bits - 8;
    const int max = (1 << t.bits) - 1;
    const bool pcLuma = RANGE[0][3] == 0;
    const int yBias = RANGE[0][3] << sh;

    for (int y = r.top; y < r.bottom; y++) {
        const BYTE* s = src + pitch * y + r.left * 4;
        T* d = Row<T>(t.data[0], t.stride[0], y) + r.left;
        for (int x = r.left; x < r.right; x++, s += 4, d++) {
            if (s[3] < 0xff) {
                const int sv = pcLuma ? ScaleFull(s[1], sh) : (s[1] << sh);
                *d = (T)ClampSample(Sar8(((int)*d - yBias) * s[3]) + sv, max);
            }
        }
    }

    if (!chroma) return;

    /* Chroma: one converted U/V per horizontal pair of pixels (U in the even
     * pixel's byte 0, V in byte 4), with the pair's alpha -- or, for 4:2:0,
     * the average over two rows as upstream's YV12 blend took it. */
    for (int i = 0; i < 2; i++) {
        const int cBias = RANGE[i + 1][3] << sh;
        for (int y = r.top; y < r.bottom; y += 1 << t.subH) {
            const BYTE* s = src + pitch * y + r.left * 4;
            T* d = Row<T>(t.data[i + 1], t.stride[i + 1], y >> t.subH) + (r.left >> 1);
            for (int x = r.left; x < r.right; x += 2, s += 8, d++) {
                int ia, sc;
                if (t.subH) {
                    ia = (s[3] + s[3 + pitch] + s[7] + s[7 + pitch]) >> 2;
                    sc = (((int)s[i * 4] + s[i * 4 + pitch]) << sh) >> 1;
                } else {
                    ia = (s[3] + s[7]) >> 1;
                    sc = s[i * 4] << sh;
                }
                if (ia < 0xff)
                    *d = (T)ClampSample(Sar8(((int)*d - cBias) * ia) + sc, max);
            }
        }
    }
}

void BlendRgbPlanes8(const MemSubPic::Planes& t, const BYTE* src, int pitch, const CRect& r)
{
    /* AlphaBlt's MSP_RGB32 formula on a DWORD gathered from the planes, which
     * is how upstream's VapourSynth filter rendered RGB24 clips. */
    for (int y = r.top; y < r.bottom; y++) {
        const BYTE* s = src + pitch * y + r.left * 4;
        BYTE* pr = Row<BYTE>(t.data[0], t.stride[0], y);
        BYTE* pg = Row<BYTE>(t.data[1], t.stride[1], y);
        BYTE* pb = Row<BYTE>(t.data[2], t.stride[2], y);
        for (int x = r.left; x < r.right; x++, s += 4) {
            if (s[3] < 0xff) {
                const DWORD sp = *(const DWORD*)s;
                const DWORD d = (DWORD)pb[x] | ((DWORD)pg[x] << 8) | ((DWORD)pr[x] << 16);
                const DWORD o = (((((d & 0x00ff00ff) * s[3]) >> 8) + (sp & 0x00ff00ff)) & 0x00ff00ff)
                              | (((((d & 0x0000ff00) * s[3]) >> 8) + (sp & 0x0000ff00)) & 0x0000ff00);
                pb[x] = (BYTE)o;
                pg[x] = (BYTE)(o >> 8);
                pr[x] = (BYTE)(o >> 16);
            }
        }
    }
}

void BlendRgbPlanes16(const MemSubPic::Planes& t, const BYTE* src, int pitch, const CRect& r)
{
    const int sh = t.bits - 8;
    const int max = (1 << t.bits) - 1;
    for (int y = r.top; y < r.bottom; y++) {
        const BYTE* s = src + pitch * y + r.left * 4;
        uint16_t* p[3] = {
            Row<uint16_t>(t.data[0], t.stride[0], y),       // R
            Row<uint16_t>(t.data[1], t.stride[1], y),       // G
            Row<uint16_t>(t.data[2], t.stride[2], y),       // B
        };
        for (int x = r.left; x < r.right; x++, s += 4) {
            if (s[3] < 0xff) {
                for (int c = 0; c < 3; c++) {
                    const int sv = ScaleFull(s[2 - c], sh);
                    p[c][x] = (uint16_t)ClampSample((((int)p[c][x] * s[3]) >> 8) + sv, max);
                }
            }
        }
    }
}

} // namespace

bool MemSubPic::AlphaBltPlanes(const Planes& t) const
{
    if (t.bits < 8 || t.bits > 16) return false;
    if (m_rcDirty.IsRectEmpty()) return true;

    const BYTE* src = (const BYTE*)m_spd.bits;
    const int pitch = m_spd.pitch;

    if (t.family == PLANES_RGB) {
        if (m_spd.type != MSP_RGB32) return false;
        if (t.bits == 8) BlendRgbPlanes8(t, src, pitch, m_rcDirty);
        else BlendRgbPlanes16(t, src, pitch, m_rcDirty);
        return true;
    }

    /* Unlock aligned the dirty area to whole chroma samples: horizontally for
     * every YUV type, vertically as well for the planar (4:2:0) ones. */
    if (!IsYuv(m_spd.type)) return false;
    if (t.family == PLANES_YUV) {
        if (t.subW != 1 || t.subH < 0 || t.subH > 1) return false;
        if (t.subH == 1 && !IsPlanar(m_spd.type)) return false;
    }

    std::lock_guard<std::recursive_mutex> lock(s_colorConvLock);
    ColorConvInitOther(m_matrix, m_range);

    const bool chroma = t.family == PLANES_YUV;
    if (t.bits == 8) BlendYuvPlanes<BYTE>(t, src, pitch, m_rcDirty, chroma);
    else BlendYuvPlanes<uint16_t>(t, src, pitch, m_rcDirty, chroma);
    return true;
}

bool IsSupportedSubPicType(int type)
{
    return type == MSP_RGB32 || type == MSP_RGBA || type == MSP_RGB24 || IsYuv(type);
}

// ---------------------------------------------------------------------------

MemSubPic::MemSubPic(int width, int height, int type, int matrix, int range)
    : m_pixels((size_t)width * height, 0xFF000000u)
    , m_matrix(matrix)
    , m_range(range)
{
    m_spd.type = type;
    m_spd.w = width;
    m_spd.h = height;
    m_spd.bpp = 32;
    m_spd.pitch = width * 4;
    m_spd.bits = m_pixels.data();
    m_spd.vidrect.left = m_spd.vidrect.top = 0;
    m_spd.vidrect.right = width;
    m_spd.vidrect.bottom = height;

    /* A fresh subpicture is fully dirty, so the first Lock clears it all. */
    m_rcDirty.SetRect(0, 0, width, height);
}

SubPicDesc& MemSubPic::Lock()
{
    // ClearDirtyRect(0xFF000000)
    if (!m_rcDirty.IsRectEmpty()) {
        BYTE* p = (BYTE*)m_spd.bits + m_spd.pitch * m_rcDirty.top + m_rcDirty.left * 4;
        for (int j = 0, h = m_rcDirty.Height(); j < h; j++, p += m_spd.pitch) {
            DWORD* row = (DWORD*)p;
            for (int x = 0, w = m_rcDirty.Width(); x < w; x++) row[x] = 0xFF000000u;
        }
        m_rcDirty.SetRectEmpty();
    }
    return m_spd;
}

void MemSubPic::Unlock(const RECT& dirty)
{
    m_rcDirty = dirty;
    if (m_rcDirty.IsRectEmpty()) return;

    std::lock_guard<std::recursive_mutex> lock(s_colorConvLock);

    if (IsYuv(m_spd.type)) {
        ColorConvInitOther(m_matrix, m_range);

        m_rcDirty.left &= ~1;
        m_rcDirty.right = (m_rcDirty.right + 1) & ~1;
        if (IsPlanar(m_spd.type)) {
            m_rcDirty.top &= ~1;
            m_rcDirty.bottom = (m_rcDirty.bottom + 1) & ~1;
        }
        /* Upstream did not clamp: YUV canvases are always even-sized, so the
         * widened rectangle cannot leave them. Clamp anyway, it is free. */
        if (m_rcDirty.right > m_spd.w) m_rcDirty.right = m_spd.w & ~1;
        if (m_rcDirty.bottom > m_spd.h) m_rcDirty.bottom = m_spd.h & ~1;
    }

    if (!IsYuv(m_spd.type)) return;

    const int w = m_rcDirty.Width(), h = m_rcDirty.Height();
    BYTE* top = (BYTE*)m_spd.bits + m_spd.pitch * m_rcDirty.top + m_rcDirty.left * 4;
    BYTE* bottom = top + m_spd.pitch * h;

    for (; top < bottom; top += m_spd.pitch) {
        BYTE* s = top;
        BYTE* e = s + w * 4;
        for (; s < e; s += 8) {             // ARGB ARGB -> AxYU AxYV
            if ((s[3] + s[7]) < 0x1fe) {
                s[1] = (BYTE)((c2y_yb[s[0]] + c2y_yg[s[1]] + c2y_yr[s[2]] + (RANGE[0][3] << 16) + 0x8000) >> 16);
                s[5] = (BYTE)((c2y_yb[s[4]] + c2y_yg[s[5]] + c2y_yr[s[6]] + (RANGE[0][3] << 16) + 0x8000) >> 16);

                int scaled_y = (s[1] + s[5] - RANGE[0][3] * 2) * cy_cy2;

                s[0] = Clip[(((((s[0] + s[4]) << 15) - scaled_y) >> 10) * c2y_cu + (RANGE[1][3] << 16) + 0x8000) >> 16];
                s[4] = Clip[(((((s[2] + s[6]) << 15) - scaled_y) >> 10) * c2y_cv + (RANGE[2][3] << 16) + 0x8000) >> 16];
            } else {
                s[1] = s[5] = (BYTE)RANGE[0][3];
                s[0] = (BYTE)RANGE[1][3];
                s[4] = (BYTE)RANGE[2][3];
            }
        }
    }
}

bool MemSubPic::AlphaBlt(SubPicDesc& target) const
{
    const SubPicDesc& src = m_spd;
    SubPicDesc dst = target;            // copy: the flip below edits it

    if (src.type != dst.type) return false;
    if (m_rcDirty.IsRectEmpty()) return true;

    CRect rs(m_rcDirty), rd(m_rcDirty);

    if (dst.h < 0) {
        dst.h = -dst.h;
        rd.bottom = dst.h - rd.bottom;
        rd.top = dst.h - rd.top;
    }

    const int w = rs.Width(), h = rs.Height();

    std::lock_guard<std::recursive_mutex> lock(s_colorConvLock);
    if (IsYuv(dst.type)) ColorConvInitOther(m_matrix, m_range);

    BYTE* s = (BYTE*)src.bits + src.pitch * rs.top + rs.left * 4;
    BYTE* d = (BYTE*)dst.bits + dst.pitch * rd.top + ((rd.left * dst.bpp) >> 3);

    if (rd.top > rd.bottom) {
        if (dst.type == MSP_RGB32 || dst.type == MSP_RGB24 || dst.type == MSP_YUY2)
            d = (BYTE*)dst.bits + dst.pitch * (rd.top - 1) + (rd.left * dst.bpp >> 3);
        else if (IsPlanar(dst.type))
            d = (BYTE*)dst.bits + dst.pitch * (rd.top - 1) + (rd.left * 8 >> 3);
        else
            return false;
        dst.pitch = -dst.pitch;
    }

    const bool sse2 = (g_cpuid.m_flags & CCpuID::sse2) != 0;

    for (int j = 0; j < h; j++, s += src.pitch, d += dst.pitch) {
        if (dst.type == MSP_RGBA) {
            /* MaskSubMod's output: see ToMaskSubRgba in the BDN writer for
             * why this exact formula (A+1 divisor, truncation, no clamp). */
            const BYTE* s2 = s;
            const BYTE* s2end = s2 + w * 4;
            DWORD* d2 = (DWORD*)d;
            for (; s2 < s2end; s2 += 4, d2++) {
                if (s2[3] < 0xff) {
                    const DWORD sp = *(const DWORD*)s2;
                    const DWORD bd = 0x00000100 - (DWORD)s2[3];
                    const DWORD B = ((sp & 0x000000ff) << 8) / bd;
                    const DWORD V = ((sp & 0x0000ff00) / bd) << 8;
                    const DWORD R = (((sp & 0x00ff0000) >> 8) / bd) << 16;
                    *d2 = B | V | R | ((0xff000000 - (sp & 0xff000000)) & 0xff000000);
                }
            }
        } else if (dst.type == MSP_RGB32) {
            const BYTE* s2 = s;
            const BYTE* s2end = s2 + w * 4;
            DWORD* d2 = (DWORD*)d;
            for (; s2 < s2end; s2 += 4, d2++) {
                if (s2[3] < 0xff) {
                    const DWORD sp = *(const DWORD*)s2;
                    /* Upstream's precedence: (a + b) & mask. Note it also
                     * clears the destination's alpha byte. */
                    *d2 = ((((*d2 & 0x00ff00ff) * s2[3]) >> 8) + (sp & 0x00ff00ff) & 0x00ff00ff)
                        | ((((*d2 & 0x0000ff00) * s2[3]) >> 8) + (sp & 0x0000ff00) & 0x0000ff00);
                }
            }
        } else if (dst.type == MSP_RGB24) {
            const BYTE* s2 = s;
            const BYTE* s2end = s2 + w * 4;
            BYTE* d2 = d;
            for (; s2 < s2end; s2 += 4, d2 += 3) {
                if (s2[3] < 0xff) {
                    d2[0] = (BYTE)(((d2[0] * s2[3]) >> 8) + s2[0]);
                    d2[1] = (BYTE)(((d2[1] * s2[3]) >> 8) + s2[1]);
                    d2[2] = (BYTE)(((d2[2] * s2[3]) >> 8) + s2[2]);
                }
            }
        } else if (dst.type == MSP_YUY2) {
            DWORD* d2 = (DWORD*)d;
            const BYTE* s2 = s;
            const BYTE* s2end = s2 + w * 4;

            for (; s2 < s2end; s2 += 8, d2++) {
                unsigned int ia = (s2[3] + s2[7]) >> 1;
                if (ia >= 0xff) continue;

                const unsigned int c = ((unsigned)s2[4] << 24) | ((unsigned)s2[5] << 16)
                                     | ((unsigned)s2[0] << 8) | s2[1];   // v y2 u y1
#if defined(VSFM_HAVE_SSE2)
                if (sse2) {
                    const unsigned int a4 = (ia << 24) | ((unsigned)s2[7] << 16) | (ia << 8) | s2[3];
                    __m128i mm_zero = _mm_setzero_si128();
                    /* 0x0080001000800010 in the low 64 bits. _mm_cvtsi64_si128,
                     * which upstream used, exists only on x86-64. */
                    __m128i mm_8181 = _mm_set_epi32(0, 0, 0x00800010, 0x00800010);
                    __m128i mm_c = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)c), mm_zero);
                    __m128i mm_d = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)*d2), mm_zero);
                    __m128i mm_a = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)a4), mm_zero);
                    mm_a = _mm_srli_epi16(mm_a, 1);
                    mm_d = _mm_sub_epi16(mm_d, mm_8181);
                    mm_d = _mm_mullo_epi16(mm_d, mm_a);
                    mm_d = _mm_srai_epi16(mm_d, 7);
                    mm_d = _mm_adds_epi16(mm_d, mm_c);
                    mm_d = _mm_packus_epi16(mm_d, mm_d);
                    *d2 = (DWORD)_mm_cvtsi128_si32(mm_d);
                    continue;
                }
#endif
                (void)sse2;
                const DWORD dv = *d2;
                const BYTE y1 = BlendYuy2Channel((int)(dv & 0xff),         0x10, s2[3], s2[1]);
                const BYTE uu = BlendYuy2Channel((int)((dv >> 8) & 0xff),  0x80, (int)ia, s2[0]);
                const BYTE y2 = BlendYuy2Channel((int)((dv >> 16) & 0xff), 0x10, s2[7], s2[5]);
                const BYTE vv = BlendYuy2Channel((int)((dv >> 24) & 0xff), 0x80, (int)ia, s2[4]);
                *d2 = (DWORD)y1 | ((DWORD)uu << 8) | ((DWORD)y2 << 16) | ((DWORD)vv << 24);
            }
        } else if (IsPlanar(dst.type)) {
            const BYTE* s2 = s;
            const BYTE* s2end = s2 + w * 4;
            BYTE* d2 = d;
            for (; s2 < s2end; s2 += 4, d2++) {
                if (s2[3] < 0xff)
                    d2[0] = (BYTE)((((d2[0] - RANGE[0][3]) * s2[3]) >> 8) + s2[1]);
            }
        } else {
            return false;
        }
    }

    dst.pitch = std::abs(dst.pitch);

    if (IsPlanar(dst.type)) {
        const int h2 = h / 2;

        if (!dst.pitchUV) dst.pitchUV = dst.pitch / 2;

        const BYTE* ss[2];
        ss[0] = (const BYTE*)src.bits + src.pitch * rs.top + rs.left * 4;
        ss[1] = ss[0] + 4;

        if (!dst.bitsU || !dst.bitsV) {
            dst.bitsU = (BYTE*)dst.bits + dst.pitch * dst.h;
            dst.bitsV = dst.bitsU + dst.pitchUV * dst.h / 2;
            if (dst.type == MSP_YV12) std::swap(dst.bitsU, dst.bitsV);
        }

        BYTE* dd[2];
        dd[0] = dst.bitsU + dst.pitchUV * rd.top / 2 + rd.left / 2;
        dd[1] = dst.bitsV + dst.pitchUV * rd.top / 2 + rd.left / 2;

        if (rd.top > rd.bottom) {
            dd[0] = dst.bitsU + dst.pitchUV * (rd.top / 2 - 1) + rd.left / 2;
            dd[1] = dst.bitsV + dst.pitchUV * (rd.top / 2 - 1) + rd.left / 2;
            dst.pitchUV = -dst.pitchUV;
        }

        for (int i = 0; i < 2; i++) {
            const BYTE* sp = ss[i];
            BYTE* dp = dd[i];
            const BYTE* is = ss[1 - i];
            for (int j = 0; j < h2; j++, sp += src.pitch * 2, dp += dst.pitchUV, is += src.pitch * 2) {
                const BYTE* s2 = sp;
                const BYTE* s2end = s2 + w * 4;
                BYTE* d2 = dp;
                const BYTE* is2 = is;
                for (; s2 < s2end; s2 += 8, d2++, is2 += 8) {
                    const unsigned int ia = (s2[3] + s2[3 + src.pitch] + is2[3] + is2[3 + src.pitch]) >> 2;
                    if (ia < 0xff)
                        *d2 = (BYTE)((((*d2 - RANGE[i + 1][3]) * (int)ia) >> 8) + ((s2[0] + s2[src.pitch]) >> 1));
                }
            }
        }
    }

    return true;
}

} // namespace vsfm
