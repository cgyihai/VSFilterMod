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

#include "../compat/vsfm_compat.h"
#include "../compat/det_float.h"
#include "../compat/det_math.h"
#include <cmath>
#include <math.h>
#include <time.h>
#include <algorithm>
#include <map>
#include <vector>
#include "RTS.h"

/* There used to be a process-wide screen-compatible HDC here, with a refcount,
 * that every CMyFont was selected into -- the reason the header warned that
 * only one RTS could run at a time. Glyphs now come from vsfm::FontEngine,
 * which owns its own cache and locking. */

static long revcolor(long c)
{
    return ((c & 0xff0000) >> 16) + (c & 0xff00) + ((c & 0xff) << 16);
}

/* \glow is an output-pixel raster effect, like \blur: while it is parsed the
 * radius is in user pixels, and GetSubtitle scales it to the renderer's
 * eighth-pixel units. These helpers are only used after that scaling. */
static int EffectiveGlowRadius(const STSStyle& style)
{
    if(!(style.mod_glowRadius > 0) || !(style.mod_glowStrength > 0)) return 0;
    const double pixels = style.mod_glowRadius * (1.0 / 8.0);
    if(!(pixels > 0)) return 0;
    if(pixels >= 4096) return 4096;
    return (int)(pixels + 0.5);
}

static int GlowStrengthQ8(const STSStyle& style)
{
    if(!(style.mod_glowStrength > 0)) return 0;
    const double strength = style.mod_glowStrength > 64 ? 64 : style.mod_glowStrength;
    return (int)(strength * 256 + 0.5);
}

namespace
{

/* 8-bit emitter canvas. The rasterizer paints body/outline coverage into it
 * in the renderer's native inverted-alpha format; it is only the emitter
 * shape, the glow itself is computed in the 16-bit plane below. */
struct GlowCanvas
{
    int w = 0;
    int h = 0;
    std::vector<unsigned int> pixels;
    SubPicDesc desc;

    GlowCanvas(int width, int height) : w(width), h(height)
    {
        pixels.assign((size_t)w * h, 0xff000000); // pixmix uses inverted alpha
        memset(&desc, 0, sizeof(desc));
        desc.w = w;
        desc.h = h;
        desc.bpp = 32;
        desc.pitch = w * 4;
        desc.bits = (BYTE*)pixels.data();
        desc.type = MSP_RGB32;
    }

    void ConvertToPremultipliedAlpha()
    {
        for(unsigned int& pixel : pixels)
        {
            const unsigned int alpha = 0xff - ((pixel >> 24) & 0xff);
            pixel = (pixel & 0x00ffffff) | (alpha << 24);
        }
    }
};

#ifdef _VSMOD // patch m016. track matte
/* Extract 6-bit coverage (0..64) from an inverted-alpha canvas, matching the
 * vector clip mask format so the matte can modulate per-pixel like any other
 * coverage source. Inverse mattes flip the coverage. */
static void ExtractMatteCoverage(const std::vector<unsigned int>& pixels, std::vector<BYTE>& coverage, bool inverse)
{
    const size_t n = pixels.size();
    coverage.resize(n);
    for(size_t i = 0; i < n; i++)
    {
        const unsigned int opacity = 0xff - (pixels[i] >> 24);
        unsigned int cov = (opacity + 2) >> 2;
        if(cov > 64) cov = 64;
        coverage[i] = (BYTE)(inverse ? 64 - cov : cov);
    }
}

/* Composite a matted event's offscreen canvas into the real frame, modulated
 * by the matte coverage. Both buffers are inverted-alpha premultiplied; the
 * matte scales both the color contribution and the coverage of each pixel,
 * which preserves the color ratio exactly for premultiplied content.
 * Clamped to bbox (the event's painted region) and the frame. */
static void CompositeMattedCanvas(const std::vector<unsigned int>& scratch, const std::vector<BYTE>& coverage,
                                  SubPicDesc& spd, const CRect& bbox)
{
    if(bbox.IsRectEmpty()) return;
    const int left = (std::max)(0, (int)bbox.left);
    const int top = (std::max)(0, (int)bbox.top);
    const int right = (std::min)((int)spd.w, (int)bbox.right);
    const int bottom = (std::min)((int)spd.h, (int)bbox.bottom);
    for(int y = top; y < bottom; y++)
    {
        const unsigned int* srow = &scratch[(size_t)y * spd.w];
        const BYTE* crow = &coverage[(size_t)y * spd.w];
        unsigned int* drow = (unsigned int*)((BYTE*)spd.bits + y * spd.pitch);
        for(int x = left; x < right; x++)
        {
            const unsigned int spx = srow[x];
            const unsigned int opacity = 0xff - (spx >> 24);
            if(!opacity) continue;
            const unsigned int scale = crow[x];
            if(!scale) continue;
            const unsigned int sopacity = (opacity * scale + 32) >> 6;
            if(!sopacity) continue;
            const unsigned int sb = ((spx & 0xff) * scale + 32) >> 6;
            const unsigned int sg = (((spx >> 8) & 0xff) * scale + 32) >> 6;
            const unsigned int sr = (((spx >> 16) & 0xff) * scale + 32) >> 6;
            const unsigned int inv = 255 - sopacity;
            const unsigned int dpx = drow[x];
            unsigned int db = sb + ((dpx & 0xff) * inv + 127) / 255;
            unsigned int dg = sg + (((dpx >> 8) & 0xff) * inv + 127) / 255;
            unsigned int dr = sr + (((dpx >> 16) & 0xff) * inv + 127) / 255;
            unsigned int da = ((dpx >> 24) * inv + 127) / 255;
            if(db > 255) db = 255;
            if(dg > 255) dg = 255;
            if(dr > 255) dr = 255;
            drow[x] = db | (dg << 8) | (dr << 16) | (da << 24);
        }
    }
}
#endif

/* Bloom-style emission plane in 16-bit premultiplied BGRA. Gaussian
 * approximations at three radii are accumulated additively: a hot tight
 * core, a mid component and a wide soft halo. The 16-bit pipeline keeps the
 * long faint tail smooth instead of quantizing it away the way the 8-bit
 * multi-pass blur did, which is what made the glow look like a flat sticker
 * outline. The plane covers only the glow region of interest, so memory
 * scales with the subtitle rather than the whole frame. */
struct GlowPlane16
{
    int ox = 0, oy = 0, w = 0, h = 0;
    std::vector<uint16_t> pixels;
    std::vector<uint16_t> work;
    std::vector<uint32_t> acc;

    void Setup(int x, int y, int width, int height)
    {
        ox = x; oy = y; w = width; h = height;
        const size_t n = (size_t)w * h * 4;
        pixels.resize(n);
        work.resize(n);
        acc.assign(n, 0);
    }

    void LoadSource(const GlowCanvas& src)
    {
        for(int y = 0; y < h; y++)
        {
            const unsigned int* srow = &src.pixels[(size_t)(oy + y) * src.w + ox];
            uint16_t* dst = &pixels[(size_t)y * w * 4];
            for(int x = 0; x < w; x++)
            {
                const unsigned int p = srow[x];
                dst[x * 4 + 0] = (uint16_t)((p & 0xff) * 257);
                dst[x * 4 + 1] = (uint16_t)(((p >> 8) & 0xff) * 257);
                dst[x * 4 + 2] = (uint16_t)(((p >> 16) & 0xff) * 257);
                dst[x * 4 + 3] = (uint16_t)(((p >> 24) & 0xff) * 257);
            }
        }
    }

    /* Linear-light accumulation in 32-bit fixed point: blur output scaled by
     * the Q8 weight without reduction, so the full exposure range survives
     * for tone mapping. Maximum is 65535 x 16384 (strength 64), well within
     * uint32 range. */
    void Accumulate(int weightQ8)
    {
        for(size_t i = 0; i < acc.size(); i++)
            acc[i] += (uint32_t)pixels[i] * (uint32_t)weightQ8;
    }
};

static void BoxBlurHorizontal16(const std::vector<uint16_t>& src, std::vector<uint16_t>& dst,
                                int w, int h, int radius)
{
    if(radius <= 0)
    {
        for(size_t i = 0, n = (size_t)w * h * 4; i < n; i++) dst[i] = src[i];
        return;
    }

    const unsigned int divisor = (unsigned int)(radius * 2 + 1);
    for(int y = 0; y < h; y++)
    {
        const size_t row = (size_t)y * w;
        unsigned int sums[4] = {0, 0, 0, 0};
        for(int k = -radius; k <= radius; k++)
        {
            const int x = k;
            if(x >= 0 && x < w)
                for(int c = 0; c < 4; c++) sums[c] += src[(row + x) * 4 + c];
        }

        for(int x = 0; x < w; x++)
        {
            for(int c = 0; c < 4; c++)
                dst[(row + x) * 4 + c] = (uint16_t)((sums[c] + divisor / 2) / divisor);
            const int addX = x + radius + 1;
            const int removeX = x - radius;
            if(addX >= 0 && addX < w)
                for(int c = 0; c < 4; c++) sums[c] += src[(row + addX) * 4 + c];
            if(removeX >= 0 && removeX < w)
                for(int c = 0; c < 4; c++) sums[c] -= src[(row + removeX) * 4 + c];
        }
    }
}

static void BoxBlurVertical16(const std::vector<uint16_t>& src, std::vector<uint16_t>& dst,
                              int w, int h, int radius)
{
    if(radius <= 0)
    {
        for(size_t i = 0, n = (size_t)w * h * 4; i < n; i++) dst[i] = src[i];
        return;
    }

    const unsigned int divisor = (unsigned int)(radius * 2 + 1);
    for(int x = 0; x < w; x++)
    {
        unsigned int sums[4] = {0, 0, 0, 0};
        for(int k = -radius; k <= radius; k++)
        {
            const int y = k;
            if(y >= 0 && y < h)
                for(int c = 0; c < 4; c++) sums[c] += src[((size_t)y * w + x) * 4 + c];
        }

        for(int y = 0; y < h; y++)
        {
            for(int c = 0; c < 4; c++)
                dst[((size_t)y * w + x) * 4 + c] = (uint16_t)((sums[c] + divisor / 2) / divisor);
            const int addY = y + radius + 1;
            const int removeY = y - radius;
            if(addY >= 0 && addY < h)
                for(int c = 0; c < 4; c++) sums[c] += src[((size_t)addY * w + x) * 4 + c];
            if(removeY >= 0 && removeY < h)
                for(int c = 0; c < 4; c++) sums[c] -= src[((size_t)removeY * w + x) * 4 + c];
        }
    }
}

static void BlurGlowPlane16(GlowPlane16& plane, int radius)
{
    if(radius <= 0) return;

    /* Three zero-padded box blurs are a close integer approximation of a
     * Gaussian kernel. Their cost is independent of the blur radius. */
    const int radii[3] = {(radius + 2) / 3, (radius + 1) / 3, radius / 3};
    for(int pass = 0; pass < 3; pass++)
    {
        BoxBlurHorizontal16(plane.pixels, plane.work, plane.w, plane.h, radii[pass]);
        BoxBlurVertical16(plane.work, plane.pixels, plane.w, plane.h, radii[pass]);
    }
}

static BYTE GlowClipAlpha(const BYTE* pAlphaMask, int maskWidth, const MOD_MOVEVC& mod_vc, int x, int y)
{
    /* Rasterizer::Draw historically samples a vector clip one row lower than
     * the nominal frame coordinate. Use the same mapping here so the glow is
     * clipped to exactly the region in which its source text was allowed to
     * render. */
    if(!mod_vc.enable)
    {
        int rows = mod_vc.spd.cy;
        if(mod_vc.size.cy > 0 && mod_vc.size.cy < rows) rows = mod_vc.size.cy;
        if(rows <= 0) return 0;
        int my = y + 1;
        if(my >= rows) my = rows - 1;
        return pAlphaMask[(size_t)my * maskWidth + x];
    }

    const int mx = x - mod_vc.pos.x;
    const int my = y + 1 - mod_vc.pos.y;
    if(mx < 0 || mx >= mod_vc.size.cx || my < 0 || my >= mod_vc.size.cy)
        return mod_vc.isInverse ? 0x40 : 0;
    return pAlphaMask[(size_t)my * mod_vc.size.cx + mx];
}

static void CompositeGlowPlane(const GlowPlane16& plane, SubPicDesc& spd, const CRect& roi,
                               int strengthQ8, const BYTE* pAlphaMask, const MOD_MOVEVC& mod_vc)
{
    for(int y = roi.top; y < roi.bottom; y++)
    {
        unsigned int* dstrow = (unsigned int*)((BYTE*)spd.bits + y * spd.pitch);
        const size_t lrow = (size_t)(y - plane.oy) * plane.w;
        for(int x = roi.left; x < roi.right; x++)
        {
            int pixelStrengthQ8 = strengthQ8;
            if(pAlphaMask)
            {
                /* Clipper masks use the rasterizer's 6-bit coverage scale. */
                pixelStrengthQ8 = (strengthQ8 * GlowClipAlpha(pAlphaMask, spd.w, mod_vc, x, y) + 32) >> 6;
                if(!pixelStrengthQ8) continue;
            }

            const size_t idx = (lrow + (x - plane.ox)) * 4;

            /* Reinhard tone mapping compresses the emission asymptotically
             * instead of clipping it, so the glow always keeps a visible
             * brightness gradient instead of saturating into a flat cloud.
             * GLOW_FULLSCALE is intensity 1.0 at strength 1; the shoulder
             * sits at a quarter of that. The hue-preserving scale factor of
             * the alpha channel is applied to the premultiplied colours. */
            const unsigned long long fullscale = 65535ull * 256ull;
            const unsigned long long shoulder = fullscale * 15 / 100;
            unsigned long long xa = plane.acc[idx + 3];
            unsigned long long xb = plane.acc[idx + 0];
            unsigned long long xg = plane.acc[idx + 1];
            unsigned long long xr = plane.acc[idx + 2];
            if(pixelStrengthQ8 < 256)
            {
                xa = xa * pixelStrengthQ8 >> 8;
                xb = xb * pixelStrengthQ8 >> 8;
                xg = xg * pixelStrengthQ8 >> 8;
                xr = xr * pixelStrengthQ8 >> 8;
            }
            if(!xa) continue;

            const int sa = (int)((xa * 255 + (xa + shoulder) / 2) / (xa + shoulder));
            if(!sa) continue;
            /* Blur rounding can make a colour channel exceed alpha by one
             * least-significant unit; clamp so packing never carries. */
            const int sb = (std::min)(255, (int)((xb * sa + xa / 2) / xa));
            const int sg = (std::min)(255, (int)((xg * sa + xa / 2) / xa));
            const int sr = (std::min)(255, (int)((xr * sa + xa / 2) / xa));

            unsigned int& dstpixel = dstrow[x];
            const unsigned int dst = dstpixel;
            const int invsa = 0xff - sa;
            const int b = (std::min)(255, (int)((dst & 0xff) * invsa >> 8) + sb);
            const int g = (std::min)(255, (int)(((dst >> 8) & 0xff) * invsa >> 8) + sg);
            const int r = (std::min)(255, (int)(((dst >> 16) & 0xff) * invsa >> 8) + sr);
            const int a = (int)(((dst >> 24) & 0xff) * invsa >> 8);
            dstpixel = (unsigned int)b | ((unsigned int)g << 8) | ((unsigned int)r << 16) | ((unsigned int)a << 24);
        }
    }
}


/* ---- patch m014. effect package helpers ---- */

static int EffectiveIglowRadius(const STSStyle& style)
{
    if(!(style.mod_iglowRadius > 0) || !(style.mod_iglowStrength > 0)) return 0;
    const double pixels = style.mod_iglowRadius * (1.0 / 8.0);
    if(!(pixels > 0)) return 0;
    if(pixels >= 4096) return 4096;
    return (int)(pixels + 0.5);
}

static int IglowStrengthQ8(const STSStyle& style)
{
    if(!(style.mod_iglowStrength > 0)) return 0;
    const double strength = style.mod_iglowStrength > 64 ? 64 : style.mod_iglowStrength;
    return (int)(strength * 256 + 0.5);
}

/* The composite-time inner glow colour in pixel byte order (blue in the low
 * byte), ready for blending. White is the default: the primary colour would
 * be invisible on a fill that is already that colour. */
static unsigned int IglowEmissionColor(const STSStyle& style)
{
    return (unsigned int)revcolor(style.mod_iglowColorOverride ? style.mod_iglowColor : 0xffffff) & 0xffffff;
}

static bool WordMatchesFilter(const STSStyle& style, const PaintPassFilter& filter)
{
    if(filter.radius < 0) return true;
    if(filter.inner)
    {
        return EffectiveIglowRadius(style) == filter.radius
               && IglowStrengthQ8(style) == filter.strengthQ8
               && IglowEmissionColor(style) == filter.colorKey
               && (int)style.mod_iglowAlpha == filter.alphaKey;
    }
    return EffectiveGlowRadius(style) == filter.radius
           && GlowStrengthQ8(style) == filter.strengthQ8;
}

/* Loads inverted coverage into the plane: zero inside the glyph shape, full
 * scale outside it. The canvas alpha byte is inverted alpha (255 where never
 * painted), which is exactly the inverted coverage, so this is a rescale. */
static void LoadInvertedCoverage16(GlowPlane16& plane, const GlowCanvas& src)
{
    for(int y = 0; y < plane.h; y++)
    {
        const unsigned int* srow = &src.pixels[(size_t)(plane.oy + y) * src.w + plane.ox];
        uint16_t* dst = &plane.pixels[(size_t)y * plane.w * 4];
        for(int x = 0; x < plane.w; x++)
            dst[x * 4 + 3] = dst[x * 4 + 0] = (uint16_t)(((srow[x] >> 24) & 0xff) * 257);
    }
}

/* Inner glow: light bleeding inward from the glyph edges. The classic formula
 * is coverage x blur(1 - coverage); the blurred inverted plane already holds
 * blur(1 - coverage). Zero padding beyond the region of interest cannot reach
 * the glyph because the margin exceeds the blur support, and outside the
 * coverage the mask is zero anyway. */
static void CompositeInnerGlowPlane(const GlowPlane16& plane, const GlowCanvas& src, SubPicDesc& spd,
                                    const CRect& roi, int strengthQ8, unsigned int color, int alphaKey,
                                    const BYTE* pAlphaMask, const MOD_MOVEVC& mod_vc)
{
    const int cb = color & 0xff;
    const int cg = (color >> 8) & 0xff;
    const int cr = (color >> 16) & 0xff;
    /* alphaKey is the ASS-style inverted alpha of the effect: 0 is opaque. */
    const int alphaScaleQ8 = 256 - ((alphaKey * 256 + 127) / 255);
    if(alphaScaleQ8 <= 0) return;

    const unsigned long long shoulder = 65535ull * 256ull / 2;

    for(int y = roi.top; y < roi.bottom; y++)
    {
        unsigned int* dstrow = (unsigned int*)((BYTE*)spd.bits + y * spd.pitch);
        const size_t lrow = (size_t)(y - plane.oy) * plane.w;
        const unsigned int* crow = &src.pixels[(size_t)y * src.w];
        for(int x = roi.left; x < roi.right; x++)
        {
            const int cov = 0xff - ((crow[x] >> 24) & 0xff);
            if(!cov) continue;

            int scaleQ8 = 256;
            if(pAlphaMask)
            {
                scaleQ8 = (256 * GlowClipAlpha(pAlphaMask, spd.w, mod_vc, x, y) + 32) >> 6;
                if(!scaleQ8) continue;
            }

            const size_t idx = (lrow + (x - plane.ox)) * 4;
            unsigned long long xe = plane.pixels[idx + 3];
            if(!xe) continue;
            xe = xe * (unsigned int)strengthQ8;
            if(scaleQ8 < 256) xe = xe * scaleQ8 >> 8;

            /* Same Reinhard shaping as the outer glow, with the shoulder at
             * half scale: the edge of the glyph lights up strongly and the
             * intensity rolls off smoothly towards the interior. */
            const int t = (int)((xe * 255 + (xe + shoulder) / 2) / (xe + shoulder));
            if(!t) continue;

            int sa = (t * cov + 127) / 255;
            sa = (sa * alphaScaleQ8 + 128) >> 8;
            if(!sa) continue;

            unsigned int& dstpixel = dstrow[x];
            const unsigned int dst = dstpixel;
            const int invsa = 0xff - sa;
            const int b = (std::min)(255, (int)((dst & 0xff) * invsa >> 8) + ((cb * sa + 128) >> 8));
            const int g = (std::min)(255, (int)(((dst >> 8) & 0xff) * invsa >> 8) + ((cg * sa + 128) >> 8));
            const int r = (std::min)(255, (int)(((dst >> 16) & 0xff) * invsa >> 8) + ((cr * sa + 128) >> 8));
            const int a = (int)(((dst >> 24) & 0xff) * invsa >> 8);
            dstpixel = (unsigned int)b | ((unsigned int)g << 8) | ((unsigned int)r << 16) | ((unsigned int)a << 24);
        }
    }
}

struct IGlowPass
{
    int radius;
    int strengthQ8;
    unsigned int color;
    int alphaKey;

    bool operator==(const IGlowPass& other) const
    {
        return radius == other.radius && strengthQ8 == other.strengthQ8
               && color == other.color && alphaKey == other.alphaKey;
    }
};

static void CollectIGlowPasses(CSubtitle* subtitle, std::vector<IGlowPass>& passes)
{
    passes.clear();
    POSITION linepos = subtitle->GetHeadPosition();
    while(linepos)
    {
        CLine* line = subtitle->GetNext(linepos);
        POSITION wordpos = line->GetHeadPosition();
        while(wordpos)
        {
            CWord* word = line->GetNext(wordpos);
            const int radius = EffectiveIglowRadius(word->m_style);
            if(radius <= 0) continue;
            const IGlowPass pass = {radius, IglowStrengthQ8(word->m_style),
                                    IglowEmissionColor(word->m_style), (int)word->m_style.mod_iglowAlpha};
            if(std::find(passes.begin(), passes.end(), pass) == passes.end())
                passes.push_back(pass);
        }
    }
}

/* Directional motion blur as a bitmap post-process. The surface is
 * premultiplied with inverted alpha, so a straight per-channel average is
 * correct filtering in both colour and coverage. Taps sit on a line through
 * each pixel, bilinearly sampled in fixed point; the tap offsets and weights
 * are constant, so this is a shift-and-accumulate filter. */
static void ApplyMotionBlur(SubPicDesc& spd, const CRect& contentBox, double lengthPx, double angleDeg)
{
    if(!(lengthPx >= 0.5)) return;

    int taps = (int)(lengthPx * 2 + 0.5) + 1;
    if(taps < 3) taps = 3;
    if(taps > 95) taps = 95;
    if(!(taps & 1)) taps++;

    const double rad = angleDeg * (3.14159265358979323846 / 180.0);
    const double dx = vsfm::DetCos(rad);
    const double dy = vsfm::DetSin(rad);

    const int ex = (int)(fabs(dx) * lengthPx / 2 + 1.5);
    const int ey = (int)(fabs(dy) * lengthPx / 2 + 1.5);
    CRect rr(contentBox.left - ex, contentBox.top - ey, contentBox.right + ex, contentBox.bottom + ey);
    if(rr.left < 0) rr.left = 0;
    if(rr.top < 0) rr.top = 0;
    if(rr.right > spd.w) rr.right = spd.w;
    if(rr.bottom > spd.h) rr.bottom = spd.h;
    const int rw = rr.right - rr.left;
    const int rh = rr.bottom - rr.top;
    if(rw <= 0 || rh <= 0) return;

    std::vector<unsigned int> backup((size_t)rw * rh);
    for(int y = 0; y < rh; y++)
    {
        const unsigned int* srow = (const unsigned int*)((const BYTE*)spd.bits + (rr.top + y) * spd.pitch) + rr.left;
        memcpy(&backup[(size_t)y * rw], srow, rw * 4);
    }

    /* Per-tap offsets in Q16. The +4096 bias keeps the biased sample position
     * positive so the shift is a clean floor. */
    std::vector<int> oxs(taps), oys(taps);
    for(int i = 0; i < taps; i++)
    {
        const double t = -lengthPx / 2 + lengthPx * i / (taps - 1);
        oxs[i] = (int)(dx * t * 65536 + (dx * t >= 0 ? 0.5 : -0.5));
        oys[i] = (int)(dy * t * 65536 + (dy * t >= 0 ? 0.5 : -0.5));
    }

    for(int y = 0; y < rh; y++)
    {
        unsigned int* drow = (unsigned int*)((BYTE*)spd.bits + (rr.top + y) * spd.pitch) + rr.left;
        for(int x = 0; x < rw; x++)
        {
            unsigned int acc[4] = {0, 0, 0, 0};
            unsigned int weightSum = 0;
            for(int i = 0; i < taps; i++)
            {
                const int bx = ((x + 4096) << 16) + oxs[i];
                const int by = ((y + 4096) << 16) + oys[i];
                const int ix = (bx >> 16) - 4096;
                const int iy = (by >> 16) - 4096;
                const int fx = bx & 0xffff;
                const int fy = by & 0xffff;

                /* Bilinear weights; samples outside the backup are
                 * transparent black, which contributes nothing. */
                const int wx1 = fx >> 8, wx0 = 256 - wx1;
                const int wy1 = fy >> 8, wy0 = 256 - wy1;
                const int w00 = wx0 * wy0, w10 = wx1 * wy0, w01 = wx0 * wy1, w11 = wx1 * wy1;

                for(int corner = 0; corner < 4; corner++)
                {
                    const int cxx = ix + (corner & 1);
                    const int cyy = iy + (corner >> 1);
                    const int w = corner == 0 ? w00 : corner == 1 ? w10 : corner == 2 ? w01 : w11;
                    if(!w) continue;
                    weightSum += w;
                    unsigned int pxl = 0xff000000;
                    if(cxx >= 0 && cxx < rw && cyy >= 0 && cyy < rh)
                        pxl = backup[(size_t)cyy * rw + cxx];
                    acc[0] += (pxl & 0xff) * w;
                    acc[1] += ((pxl >> 8) & 0xff) * w;
                    acc[2] += ((pxl >> 16) & 0xff) * w;
                    acc[3] += ((pxl >> 24) & 0xff) * w;
                }
            }

            if(!weightSum)
            {
                drow[x] = backup[(size_t)y * rw + x];
                continue;
            }
            const unsigned int b = (acc[0] + weightSum / 2) / weightSum;
            const unsigned int g = (acc[1] + weightSum / 2) / weightSum;
            const unsigned int r = (acc[2] + weightSum / 2) / weightSum;
            const unsigned int a = (acc[3] + weightSum / 2) / weightSum;
            drow[x] = (b & 0xff) | ((g & 0xff) << 8) | ((r & 0xff) << 16) | ((a & 0xff) << 24);
        }
    }
}

/* Bevel/emboss lighting. The coverage gradient across a pair of pixels offset
 * along the fixed upper-left light direction drives a highlight/shadow term:
 * edges facing the light brighten towards white, edges facing away darken.
 * Colour-only modulation in the premultiplied surface keeps the coverage
 * invariant, so the alpha channel is never touched. */
static void ApplyBevel(SubPicDesc& spd, const CRect& contentBox, int size, int strengthQ8)
{
    if(size <= 0 || strengthQ8 <= 0) return;

    CRect rr(contentBox.left - size, contentBox.top - size, contentBox.right + size, contentBox.bottom + size);
    if(rr.left < 0) rr.left = 0;
    if(rr.top < 0) rr.top = 0;
    if(rr.right > spd.w) rr.right = spd.w;
    if(rr.bottom > spd.h) rr.bottom = spd.h;
    const int rw = rr.right - rr.left;
    const int rh = rr.bottom - rr.top;
    if(rw <= 0 || rh <= 0) return;

    std::vector<unsigned int> backup((size_t)rw * rh);
    for(int y = 0; y < rh; y++)
    {
        const unsigned int* srow = (const unsigned int*)((const BYTE*)spd.bits + (rr.top + y) * spd.pitch) + rr.left;
        memcpy(&backup[(size_t)y * rw], srow, rw * 4);
    }

    /* The lighting term comes from the gradient of a *smoothed* coverage map.
     * Raw coverage shifts sign at every anti-aliased edge within a couple of
     * pixels, which reads as speckle rather than a pillow; blurring the map
     * first spreads the gradient into wide smooth lighting zones. */
    std::vector<BYTE> covmask((size_t)rw * rh);
    for(int y = 0; y < rh; y++)
        for(int x = 0; x < rw; x++)
            covmask[(size_t)y * rw + x] = (BYTE)(0xff - ((backup[(size_t)y * rw + x] >> 24) & 0xff));
    {
        /* small single-scale blur, radix on the bevel size; scalar edition of
         * the vector mask feather blur, on a copy so the alpha channel in the
         * frame itself is never touched */
        std::vector<BYTE> wmask((size_t)rw * rh);
        const int br = (std::max)(1, size / 2);
        const int radii[3] = {(br + 2) / 3, (br + 1) / 3, br / 3};
        BYTE* mask = covmask.data();
        for(int pass = 0; pass < 3; pass++)
        {
            const int r = radii[pass];
            if(r <= 0) continue;
            const unsigned int divisor = (unsigned int)(r * 2 + 1);
            for(int y = 0; y < rh; y++)
            {
                BYTE* rowp = mask + (size_t)y * rw;
                BYTE* orow = wmask.data() + (size_t)y * rw;
                unsigned int sum = 0;
                for(int k = -r; k <= r; k++)
                    if(k >= 0 && k < rw) sum += rowp[k];
                for(int x = 0; x < rw; x++)
                {
                    orow[x] = (BYTE)((sum + divisor / 2) / divisor);
                    const int addX = x + r + 1, removeX = x - r;
                    if(addX >= 0 && addX < rw) sum += rowp[addX];
                    if(removeX >= 0 && removeX < rw) sum -= rowp[removeX];
                }
            }
            for(int x = 0; x < rw; x++)
            {
                unsigned int sum = 0;
                for(int k = -r; k <= r; k++)
                    if(k >= 0 && k < rh) sum += wmask[(size_t)k * rw + x];
                for(int y = 0; y < rh; y++)
                {
                    mask[(size_t)y * rw + x] = (BYTE)((sum + divisor / 2) / divisor);
                    const int addY = y + r + 1, removeY = y - r;
                    if(addY >= 0 && addY < rh) sum += wmask[(size_t)addY * rw + x];
                    if(removeY >= 0 && removeY < rh) sum -= wmask[(size_t)removeY * rw + x];
                }
            }
        }
    }

    const auto coverageAt = [&](int x, int y) -> int
    {
        if(x < 0 || x >= rw || y < 0 || y >= rh) return 0;
        return covmask[(size_t)y * rw + x];
    };

    for(int y = 0; y < rh; y++)
    {
        unsigned int* drow = (unsigned int*)((BYTE*)spd.bits + (rr.top + y) * spd.pitch) + rr.left;
        for(int x = 0; x < rw; x++)
        {
            const int emboss = coverageAt(x + size, y + size) - coverageAt(x - size, y - size);
            if(!emboss) continue;
            int light = (emboss * strengthQ8 + 128) >> 8;
            if(light > 255) light = 255;
            else if(light < -255) light = -255;
            if(!light) continue;

            const unsigned int pxl = backup[(size_t)y * rw + x];
            const int cov = 0xff - ((pxl >> 24) & 0xff);
            if(!cov) continue;

            unsigned int out = pxl & 0xff000000;
            for(int c = 0; c < 3; c++)
            {
                int v = (pxl >> (c * 8)) & 0xff;
                if(light > 0)
                {
                    /* brighten towards coverage-scaled white */
                    v = v + ((cov - v) * light + 127) / 255;
                    if(v > cov) v = cov;
                }
                else
                {
                    v = (v * (255 + light) + 127) / 255;
                }
                if(v < 0) v = 0;
                out |= (unsigned int)v << (c * 8);
            }
            drow[x] = out;
        }
    }
}

/* Feathered rectangular clipping: a smoothstep alpha ramp across the feather
 * zone. For a normal clip the painting rectangle is inflated by the feather
 * width and the ramp falls off outside the nominal edge, so everything inside
 * the user rectangle stays fully visible. For an inverse clip the ramp rises
 * away from the rectangle instead. The surface is premultiplied, so scaling
 * the opacity scales the colour channels and re-factors the inverted alpha. */
static void ApplyClipFeather(SubPicDesc& spd, const CRect& nominal, double feather, bool inverse)
{
    if(!(feather > 0)) return;

    const int fext = (int)(feather + 0.5) + 1;
    CRect rr(nominal.left - fext, nominal.top - fext, nominal.right + fext, nominal.bottom + fext);
    if(rr.left < 0) rr.left = 0;
    if(rr.top < 0) rr.top = 0;
    if(rr.right > spd.w) rr.right = spd.w;
    if(rr.bottom > spd.h) rr.bottom = spd.h;
    if(rr.IsRectEmpty()) return;

    for(int y = rr.top; y < rr.bottom; y++)
    {
        unsigned int* drow = (unsigned int*)((BYTE*)spd.bits + y * spd.pitch);
        for(int x = rr.left; x < rr.right; x++)
        {
            /* Signed distance outwards from the nominal rectangle in pixels. */
            const double dx = (std::max)((double)(nominal.left - x), (double)(x - nominal.right + 1));
            const double dy = (std::max)((double)(nominal.top - y), (double)(y - nominal.bottom + 1));
            double d;
            if(dx <= 0 && dy <= 0) d = (std::max)(dx, dy);
            else if(dx <= 0) d = dy;
            else if(dy <= 0) d = dx;
            else d = vsfm::DetPow(dx * dx + dy * dy, 0.5);

            if(d <= 0) continue;
            double t = d / feather;
            if(t > 1) t = 1;

            /* smoothstep in fixed point; for a normal clip the factor falls
             * from 1 at the nominal edge, for an inverse clip it rises.
             * Pixels at or beyond the feather width scale to zero, which is
             * what erases the inflated painting ring. */
            const double ss = t * t * (3 - 2 * t);
            const int f = inverse ? (int)(ss * 256 + 0.5) : (int)((1.0 - ss) * 256 + 0.5);
            if(f >= 256) continue;

            unsigned int& pxl = drow[x];
            const unsigned int p = pxl;
            const unsigned int inv = (p >> 24) & 0xff;
            const unsigned int cov = 0xff - inv;
            const unsigned int ncov = (cov * f + 128) >> 8;
            const unsigned int b = ((p & 0xff) * f + 128) >> 8;
            const unsigned int g = (((p >> 8) & 0xff) * f + 128) >> 8;
            const unsigned int r = (((p >> 16) & 0xff) * f + 128) >> 8;
            pxl = (b & 0xff) | ((g & 0xff) << 8) | ((r & 0xff) << 16) | ((0xff - ncov) << 24);
        }
    }
}

/* Gaussian-approximate blur of a vector clip mask in place. The mask uses the
 * rasterizer's 6-bit coverage scale; three box passes keep it in that scale. */
static void FeatherVectorMaskBlur(BYTE* mask, int w, int h, int radius)
{
    if(radius <= 0 || w <= 0 || h <= 0) return;

    std::vector<BYTE> work((size_t)w * h);
    const int radii[3] = {(radius + 2) / 3, (radius + 1) / 3, radius / 3};

    for(int pass = 0; pass < 3; pass++)
    {
        const int r = radii[pass];
        if(r <= 0) continue;
        const unsigned int divisor = (unsigned int)(r * 2 + 1);

        /* horizontal */
        for(int y = 0; y < h; y++)
        {
            BYTE* row = mask + (size_t)y * w;
            BYTE* orow = work.data() + (size_t)y * w;
            unsigned int sum = 0;
            for(int k = -r; k <= r; k++)
                if(k >= 0 && k < w) sum += row[k];
            for(int x = 0; x < w; x++)
            {
                orow[x] = (BYTE)((sum + divisor / 2) / divisor);
                const int addX = x + r + 1, removeX = x - r;
                if(addX >= 0 && addX < w) sum += row[addX];
                if(removeX >= 0 && removeX < w) sum -= row[removeX];
            }
        }
        /* vertical */
        for(int x = 0; x < w; x++)
        {
            unsigned int sum = 0;
            for(int k = -r; k <= r; k++)
                if(k >= 0 && k < h) sum += work[(size_t)k * w + x];
            for(int y = 0; y < h; y++)
            {
                mask[(size_t)y * w + x] = (BYTE)((sum + divisor / 2) / divisor);
                const int addY = y + r + 1, removeY = y - r;
                if(addY >= 0 && addY < h) sum += work[(size_t)addY * w + x];
                if(removeY >= 0 && removeY < h) sum -= work[(size_t)removeY * w + x];
            }
        }
    }
}

struct GlowPass
{
    int radius;
    int strengthQ8;

    bool operator==(const GlowPass& other) const
    {
        return radius == other.radius && strengthQ8 == other.strengthQ8;
    }
};

static void CollectGlowPasses(CSubtitle* subtitle, std::vector<GlowPass>& passes)
{
    passes.clear();
    POSITION linepos = subtitle->GetHeadPosition();
    while(linepos)
    {
        CLine* line = subtitle->GetNext(linepos);
        POSITION wordpos = line->GetHeadPosition();
        while(wordpos)
        {
            CWord* word = line->GetNext(wordpos);
            const int radius = EffectiveGlowRadius(word->m_style);
            if(radius <= 0) continue;
            const GlowPass pass = {radius, GlowStrengthQ8(word->m_style)};
            if(std::find(passes.begin(), passes.end(), pass) == passes.end())
                passes.push_back(pass);
        }
    }
    std::sort(passes.begin(), passes.end(), [](const GlowPass& a, const GlowPass& b)
    {
        if(a.radius != b.radius) return a.radius < b.radius;
        return a.strengthQ8 < b.strengthQ8;
    });
}

static CRect IntersectGlowRect(const CRect& a, const CRect& b)
{
    return CRect((std::max)(a.left, b.left), (std::max)(a.top, b.top),
                 (std::min)(a.right, b.right), (std::min)(a.bottom, b.bottom));
}

} // namespace


//////////////////////////////////////////////////////////////////////////////////////////////

// CMyFont

CMyFont::CMyFont(const STSStyle& style)
    : m_ascent(0), m_descent(0)
{
    vsfm::FontDesc desc;
    desc.faceName = (LPCWSTR)style.fontName;
    /* Positive height, i.e. a GDI character *cell* height. RTS has already
     * multiplied fontSize by 64; the ">> 3" below takes it back to the 8x
     * supersampled space the rasterizer works in. */
    desc.height = vsfm::CvtTruncSd(style.fontSize + 0.5);
    desc.weight = style.fontWeight;
    desc.charSet = style.charSet;
    desc.italic = !!style.fItalic;
    desc.underline = !!style.fUnderline;
    desc.strikeOut = !!style.fStrikeOut;
#ifdef _VSMOD // patch m007. symbol rotating
    desc.orientation = style.mod_fontOrient;
#endif

    m_face = vsfm::FontEngine::Get().OpenFont(desc);

    if(!m_face)
    {
        desc.faceName = L"Arial";
        m_face = vsfm::FontEngine::Get().OpenFont(desc);
    }

    if(m_face)
    {
        m_ascent = ((m_face->Metrics().ascent + 4) >> 3);
        m_descent = ((m_face->Metrics().descent + 4) >> 3);
    }
}

// CWord

CWord::CWord(const STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley)
    : m_style(style), m_str(str)
    , m_width(0), m_ascent(0), m_descent(0)
    , m_ktype(ktype), m_kstart(kstart), m_kend(kend)
    , m_scalex(scalex), m_scaley(scaley)
    , m_fDrawn(false), m_p(INT_MAX, INT_MAX)
    , m_fLineBreak(false), m_fWhiteSpaceChar(false)
    , m_pOpaqueBox(NULL), isOpaqueBox(false)
{
    if(str.IsEmpty())
    {
        m_fWhiteSpaceChar = m_fLineBreak = true;
    }

    CMyFont font(m_style);
    m_ascent = (int)(m_style.fontScaleY / 100 * font.m_ascent);
    m_descent = (int)(m_style.fontScaleY / 100 * font.m_descent);
    m_width = 0;
}

CWord::~CWord()
{
    if(m_pOpaqueBox) delete m_pOpaqueBox;
}

bool CWord::Append(CWord* w)
{
    if(!(m_style == w->m_style)
       || m_fLineBreak || w->m_fLineBreak
       || w->m_kstart != w->m_kend || m_ktype != w->m_ktype) return(false);

    m_fWhiteSpaceChar = m_fWhiteSpaceChar && w->m_fWhiteSpaceChar;
    m_str += w->m_str;
    m_width += w->m_width;

    m_fDrawn = false;
    m_p = CPoint(INT_MAX, INT_MAX);

    return(true);
}

void CWord::Paint(CPoint p, CPoint org)
{
    if(!m_str) return;

    if(!m_fDrawn)
    {
        if(!CreatePath()) return;

        Transform(CPoint((org.x - p.x) * 8, (org.y - p.y) * 8));

        if(!ScanConvert()) return;

        if(m_style.borderStyle == 0 && (m_style.outlineWidthX + m_style.outlineWidthY > 0))
        {
            if(!CreateWidenedRegion((int)(m_style.outlineWidthX + 0.5), (int)(m_style.outlineWidthY + 0.5))) return;
        }
        else if(m_style.borderStyle == 1)
        {
            if(!CreateOpaqueBox()) return;
        }

        m_fDrawn = true;

        if(!Rasterize(p.x & 7, p.y & 7, m_style.fBlur, m_style.fGaussianBlur)) return;
    }
    else if((m_p.x & 7) != (p.x & 7) || (m_p.y & 7) != (p.y & 7))
    {
        Rasterize(p.x & 7, p.y & 7, m_style.fBlur, m_style.fGaussianBlur);
    }

    m_p = p;

    if(m_pOpaqueBox)
        m_pOpaqueBox->Paint(p, org);
}

#include "../compat/det_float.h"
#include "../compat/det_math.h"

/* The path is moved to screen space in single precision with the operations
 * of upstream's SSE code, one point at a time; see compat/det_float.h for why
 * this gives the same result on every CPU. Nothing below may be contracted
 * into an FMA: meson.build turns that off for GCC and Clang, and these cover
 * the compilers that honour a pragma for it. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

void CWord::Transform(CPoint org)
{
    using vsfm::MaxSs;
    using vsfm::MinSs;

    const double scalex = isOpaqueBox ? 1 : m_style.fontScaleX / 100;
    const double scaley = isOpaqueBox ? 1 : m_style.fontScaleY / 100;

    /* Upstream's angle conversion, 3.1415 and all; changing it would move
     * every rotated glyph. */
    const float caz = (float)vsfm::DetCos((3.1415 / 180) * m_style.fontAngleZ);
    const float saz = (float)vsfm::DetSin((3.1415 / 180) * m_style.fontAngleZ);
    const float cax = (float)vsfm::DetCos((3.1415 / 180) * m_style.fontAngleX);
    const float sax = (float)vsfm::DetSin((3.1415 / 180) * m_style.fontAngleX);
    const float cay = (float)vsfm::DetCos((3.1415 / 180) * m_style.fontAngleY);
    const float say = (float)vsfm::DetSin((3.1415 / 180) * m_style.fontAngleY);

    const float xscale = (float)scalex;
    const float yscale = (float)scaley;
    const float xshift = (float)m_style.fontShiftX;
    const float yshift = (float)m_style.fontShiftY;
    const float xorg = (float)org.x;
    const float yorg = (float)org.y;
    const float xzoomf = (float)(m_scalex * 20000.0);
    const float yzoomf = (float)(m_scaley * 20000.0);

#ifdef _VSMOD
    const double xrnd = m_style.mod_rand.X * 100;
    const double yrnd = m_style.mod_rand.Y * 100;
    const double zrnd = m_style.mod_rand.Z * 100;
    const bool is_dist = m_style.mod_distort.enabled;
    const float z0 = (float)m_style.mod_z;

    vsfm_srand(m_style.mod_rand.Seed);
#else
    const double xrnd = 0, yrnd = 0, zrnd = 0;
    const bool is_dist = false;
    const float z0 = 0;
#endif
    const bool jitter = xrnd != 0 || yrnd != 0 || zrnd != 0;

    // patch m008. distort: P = P0 + (P1 - P0)u + (P3 - P0)v + (P0 + P2 - P1 - P3)uv
    float minx = 2147483648.0f, miny = 2147483648.0f;   // (float)INT_MAX
    float xsz = 0, ysz = 0, rxsz = 0, rysz = 0;
    float dst1x = 0, dst1y = 0, dst3x = 0, dst3y = 0, dst213x = 0, dst213y = 0;

#ifdef _VSMOD
    if(is_dist)
    {
        float maxx = -2147483648.0f, maxy = -2147483648.0f;
        for(size_t i = 0; i < mPathPoints; i++)
        {
            const float px = (float)mpPathPoints[i].x;
            const float py = (float)mpPathPoints[i].y;
            minx = MinSs(minx, px);
            miny = MinSs(miny, py);
            maxx = MaxSs(maxx, px);
            maxy = MaxSs(maxy, py);
        }

        xsz = MaxSs(maxx - minx, 0.0f);
        ysz = MaxSs(maxy - miny, 0.0f);

        /* Upstream multiplies by RCPSS's estimate of 1/size rather than
         * dividing. A shape with no width or height gets an infinite
         * reciprocal and its points become NaN, which CvtTruncSs maps to
         * INT32_MIN exactly as the instruction does. */
        rxsz = vsfm::RcpIntel(xsz);
        rysz = vsfm::RcpIntel(ysz);

        dst1x = (float)m_style.mod_distort.pointsx[0];
        dst1y = (float)m_style.mod_distort.pointsy[0];
        dst3x = (float)m_style.mod_distort.pointsx[2];
        dst3y = (float)m_style.mod_distort.pointsy[2];
        dst213x = ((float)m_style.mod_distort.pointsx[1] - dst1x) - dst3x;
        dst213y = ((float)m_style.mod_distort.pointsy[1] - dst1y) - dst3y;
    }
#endif

    /* patch m003. \rnd jitter is drawn four points at a time, as upstream's
     * SSE code did: x, y and z for each of four lanes, the lanes in reverse
     * point order, and a full four draws even for a final group of fewer
     * points. The pattern a given \rnds seed produces depends on this. */
    float rx[4] = {0, 0, 0, 0}, ry[4] = {0, 0, 0, 0}, rz[4] = {0, 0, 0, 0};

    for(size_t i = 0; i < mPathPoints; i++)
    {
        const size_t lane = 3 - (i & 3);

        if(jitter && (i & 3) == 0)
        {
            for(int k = 0; k < 4; k++)
            {
                rx[k] = xrnd > 0 ? (float)(xrnd - vsfm_rand() % (int)(xrnd * 2 + 1)) : 0;
                ry[k] = yrnd > 0 ? (float)(yrnd - vsfm_rand() % (int)(yrnd * 2 + 1)) : 0;
                rz[k] = zrnd > 0 ? (float)(zrnd - vsfm_rand() % (int)(zrnd * 2 + 1)) : 0;
            }
        }

        float px = (float)mpPathPoints[i].x;
        float py = (float)mpPathPoints[i].y;
        float pz = z0;

        if(is_dist)
        {
            const float u = (px - minx) * rxsz;
            const float v = (py - miny) * rysz;

            float x = dst213x * u;
            x = x * v;
            x = x + dst3x * v;
            x = x + dst1x * u;
            x = x * xsz;
            x = x + minx;

            float y = dst213y * u;
            y = y * v;
            y = y + dst3y * v;
            y = y + dst1y * u;
            y = y * ysz;
            y = y + miny;

            px = x;
            py = y;
        }

        if(jitter)
        {
            if(xrnd != 0) px = px + rx[lane] * 0.01f;
            if(yrnd != 0) py = py + ry[lane] * 0.01f;
            if(zrnd != 0) pz = pz + rz[lane] * 0.01f;
        }

        // shear (\fax, \fay) and scale
        float tx = m_style.fontShiftX != 0 ? xshift * py + px : px;
        tx = tx * xscale - xorg;
        float ty = m_style.fontShiftY != 0 ? yshift * px + py : py;
        ty = ty * yscale - yorg;

        // \frz
        const float x1 = tx * caz + ty * saz;
        const float y1 = ty * caz - tx * saz;

        // \frx
        const float y2 = y1 * cax + pz * sax;
        const float z2 = y1 * sax - pz * cax;

        // \fry
        const float x3 = x1 * cay + z2 * say;
        const float z3 = x1 * say - z2 * cay;

        // perspective
        float x = (x3 * xzoomf) / MaxSs(z3 + xzoomf, 1000.0f);
        float y = (y2 * yzoomf) / MaxSs(z3 + yzoomf, 1000.0f);

        x = x + xorg;
        y = y + yorg;
        x = x + 0.5f;
        y = y + 0.5f;

        mpPathPoints[i].x = vsfm::CvtTruncSs(x);
        mpPathPoints[i].y = vsfm::CvtTruncSs(y);
    }
}

bool CWord::CreateOpaqueBox()
{
    if(m_pOpaqueBox) return(true);

    STSStyle style = m_style;
    style.borderStyle = 0;
    style.outlineWidthX = style.outlineWidthY = 0;
    style.colors[0] = m_style.colors[2];
    style.alpha[0] = m_style.alpha[2];

    int w = (int)(m_style.outlineWidthX + 0.5);
    int h = (int)(m_style.outlineWidthY + 0.5);

    CStringW str;
    str.Format(L"m %d %d l %d %d %d %d %d %d",
               -w, -h,
               m_width + w, -h,
               m_width + w, m_ascent + m_descent + h,
               -w, m_ascent + m_descent + h);

    m_pOpaqueBox = DNew CPolygon(style, str, 0, 0, 0, 1.0 / 8, 1.0 / 8, 0);
    m_pOpaqueBox->isOpaqueBox = true;

    return(!!m_pOpaqueBox);
}

// CText

CText::CText(const STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley)
    : CWord(style, str, ktype, kstart, kend, scalex, scaley)
{
    if(m_str == L" ")
    {
        m_fWhiteSpaceChar = true;
    }

    CMyFont font(m_style);
    if(!font.IsValid()) return;

#ifdef _VSMOD // patch m007. symbol rotating
    double t = (double)m_style.mod_fontOrient * 3.1415926 / 1800;
#endif
    /* The original also took this branch when (long)GetVersion() < 0, i.e. on
     * Windows 9x, whose GDI mismeasured whole runs. That has not been a live
     * case for two decades and there is no portable equivalent. */
    if(m_style.fontSpacing)
    {
        for(LPCWSTR s = m_str; *s; s++)
        {
            CSize extent;
            if(!font.m_face->TextExtent(s, 1, &extent))
            {
                ASSERT(0);
                return;
            }
#ifdef _VSMOD // patch m007. symbol rotating
            m_width += (int)(extent.cx * fabs(vsfm::DetCos(t)) + extent.cy * fabs(vsfm::DetSin(t)) + m_style.fontSpacing);
#else
            m_width += extent.cx + (int)m_style.fontSpacing;
#endif
        }
//			m_width -= (int)m_style.fontSpacing; // TODO: subtract only at the end of the line
    }
    else
    {
        CSize extent;
        if(!font.m_face->TextExtent(m_str, m_str.GetLength(), &extent))
        {
            ASSERT(0);
            return;
        }
#ifdef _VSMOD // patch m007. symbol rotating
        m_width += (int)(extent.cx * fabs(vsfm::DetCos(t)) + extent.cy * fabs(vsfm::DetSin(t)));
#else
        m_width += extent.cx;
#endif
    }

    m_width = (int)(m_style.fontScaleX / 100 * m_width + 4) >> 3;
}

CWord* CText::Copy()
{
    return(DNew CText(*this));
}

bool CText::Append(CWord* w)
{
    return(dynamic_cast<CText*>(w) && CWord::Append(w));
}

bool CText::CreatePath()
{
    CMyFont font(m_style);
    if(!font.IsValid()) return(false);

    vsfm::GlyphPath path;
    int width = 0;

    if(m_style.fontSpacing)
    {
        bool bFirstPath = true;

        for(LPCWSTR s = m_str; *s; s++)
        {
            CSize extent;
            if(!font.m_face->TextExtent(s, 1, &extent))
            {
                ASSERT(0);
                return(false);
            }

            /* One glyph at a time, each laid down at the running pen position
             * -- the shape of the old PartialBeginPath/TextOutW/PartialEndPath
             * loop, minus the HDC. */
            path.clear();
            font.m_face->AppendString(s, 1, 0, 0, path);
            AppendPath(path, width, 0, bFirstPath);
            bFirstPath = false;

            width += extent.cx + (int)m_style.fontSpacing;
        }
    }
    else
    {
        font.m_face->AppendString(m_str, m_str.GetLength(), 0, 0, path);
        SetPath(path);
    }

    return(true);
}

// CPolygon

CPolygon::CPolygon(const STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley, int baseline)
    : CWord(style, str, ktype, kstart, kend, scalex, scaley)
    , m_scalex(scalex), m_scaley(scaley), m_baseline(baseline)
{
    ParseStr();
}

CPolygon::~CPolygon()
{
}

CWord* CPolygon::Copy()
{
    return(DNew CPolygon(m_style, m_str, m_ktype, m_kstart, m_kend, m_scalex, m_scaley, m_baseline));
}

bool CPolygon::Append(CWord* w)
{
    int width = m_width;

    CPolygon* p = dynamic_cast<CPolygon*>(w);
    if(!p) return(false);

    // TODO
    return(false);

    return(true);
}

bool CPolygon::GetLONG(CStringW& str, LONG& ret)
{
    double dblVal = 0;
    LPWSTR s = (LPWSTR)(LPCWSTR)str, e = s;
    dblVal = vsfm::WcsToD(str, &e);
    ret = std::round(dblVal);
    str = str.Mid(e - s);
    return(e > s);
}
bool CPolygon::Get6BitFixedPoint(CStringW& str, LONG& ret)
{
    LPWSTR s = (LPWSTR)(LPCWSTR)str, e = s;
    ret = vsfm::WcsToD(str, &e) * 64;
    str.Delete(0, e - s);
    return(e > s);
}
bool CPolygon::GetPOINT(CStringW& str, POINT& ret)
{
    return(Get6BitFixedPoint(str, ret.x) && Get6BitFixedPoint(str, ret.y));
}

bool CPolygon::ParseStr()
{
    if(m_pathTypesOrg.GetCount() > 0) return(true);

    CPoint p;
    int i, j, lastsplinestart = -1, firstmoveto = -1, lastmoveto = -1;

    CStringW str = m_str;
    str.SpanIncluding(L"mnlbspc 0123456789");
    str.Replace(L"m", L"*m");
    str.Replace(L"n", L"*n");
    str.Replace(L"l", L"*l");
    str.Replace(L"b", L"*b");
    str.Replace(L"s", L"*s");
    str.Replace(L"p", L"*p");
    str.Replace(L"c", L"*c");

    int k = 0;
    for(CStringW s = str.Tokenize(L"*", k); !s.IsEmpty(); s = str.Tokenize(L"*", k))
    {
        WCHAR c = s[0];
        s.TrimLeft(L"mnlbspc ");
        switch(c)
        {
        case 'm':
            lastmoveto = m_pathTypesOrg.GetCount();
            if(firstmoveto == -1) firstmoveto = lastmoveto;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_MOVETO);
                m_pathPointsOrg.Add(p);
            }
            break;
        case 'n':
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_MOVETONC);
                m_pathPointsOrg.Add(p);
            }
            break;
        case 'l':
            if (m_pathPointsOrg.GetCount() < 1) break;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_LINETO);
                m_pathPointsOrg.Add(p);
            }
            break;
        case 'b':
            j = m_pathTypesOrg.GetCount();
            if (j < 1) break;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_BEZIERTO);
                m_pathPointsOrg.Add(p);
                j++;
            }
            j = m_pathTypesOrg.GetCount() - ((m_pathTypesOrg.GetCount() - j) % 3);
            m_pathTypesOrg.SetCount(j);
            m_pathPointsOrg.SetCount(j);
            break;
        case 's':
            if (m_pathPointsOrg.GetCount() < 1) break;
            j = lastsplinestart = m_pathTypesOrg.GetCount();
            i = 3;
            while(i-- && GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_BSPLINETO);
                m_pathPointsOrg.Add(p);
                j++;
            }
            if(m_pathTypesOrg.GetCount() - lastsplinestart < 3)
            {
                m_pathTypesOrg.SetCount(lastsplinestart);
                m_pathPointsOrg.SetCount(lastsplinestart);
                lastsplinestart = -1;
            }
            // no break here
        case 'p':
            if (m_pathPointsOrg.GetCount() < 3) break;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                m_pathPointsOrg.Add(p);
                j++;
            }
            break;
        case 'c':
            if(lastsplinestart > 0)
            {
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                p = m_pathPointsOrg[lastsplinestart-1]; // we need p for temp storage, because operator [] will return a reference to CPoint and Add() may reallocate its internal buffer (this is true for MFC 7.0 but not for 6.0, hehe)
                m_pathPointsOrg.Add(p);
                p = m_pathPointsOrg[lastsplinestart];
                m_pathPointsOrg.Add(p);
                p = m_pathPointsOrg[lastsplinestart+1];
                m_pathPointsOrg.Add(p);
                lastsplinestart = -1;
            }
            break;
        default:
            break;
        }
    }

    if(lastmoveto == -1 || firstmoveto > 0)
    {
        m_pathTypesOrg.RemoveAll();
        m_pathPointsOrg.RemoveAll();
        return(false);
    }

    int minx = INT_MAX, miny = INT_MAX, maxx = -INT_MAX, maxy = -INT_MAX;

    for(i = 0; i < m_pathTypesOrg.GetCount(); i++)
    {
        m_pathPointsOrg[i].x = (int)(m_scalex * m_pathPointsOrg[i].x);
        m_pathPointsOrg[i].y = (int)(m_scaley * m_pathPointsOrg[i].y);
        if(minx > m_pathPointsOrg[i].x) minx = m_pathPointsOrg[i].x;
        if(miny > m_pathPointsOrg[i].y) miny = m_pathPointsOrg[i].y;
        if(maxx < m_pathPointsOrg[i].x) maxx = m_pathPointsOrg[i].x;
        if(maxy < m_pathPointsOrg[i].y) maxy = m_pathPointsOrg[i].y;
    }

    m_width = std::max(maxx - minx, 0);
    m_ascent = std::max(maxy - miny, 0);

    int baseline = (int)(64 * m_scaley * m_baseline);
    m_descent = baseline;
    m_ascent -= baseline;

    m_width = ((int)(m_style.fontScaleX / 100 * m_width) + 4) >> 3;
    m_ascent = ((int)(m_style.fontScaleY / 100 * m_ascent) + 4) >> 3;
    m_descent = ((int)(m_style.fontScaleY / 100 * m_descent) + 4) >> 3;

    return(true);
}

bool CPolygon::CreatePath()
{
    int len = m_pathTypesOrg.GetCount();
    if(len == 0) return(false);

    if(mPathPoints != len)
    {
        mpPathTypes = (BYTE*)realloc(mpPathTypes, len * sizeof(BYTE));
        mpPathPoints = (POINT*)realloc(mpPathPoints, len * sizeof(POINT));
        if(!mpPathTypes || !mpPathPoints) return(false);
        mPathPoints = len;
    }

    memcpy(mpPathTypes, m_pathTypesOrg.GetData(), len * sizeof(BYTE));
    memcpy(mpPathPoints, m_pathPointsOrg.GetData(), len * sizeof(POINT));

    return(true);
}

// CClipper

CClipper::CClipper(CStringW str, CSize size, double scalex, double scaley, bool inverse)
    : CPolygon(STSStyle(), str, 0, 0, 0, scalex, scaley, 0)
{
    m_size.cx = m_size.cy = 0;
    m_pAlphaMask = NULL;

    if(size.cx < 0 || size.cy < 0 || !(m_pAlphaMask = DNew BYTE[size.cx*size.cy])) return;

    m_size = size;
    m_inverse = inverse;

    memset(m_pAlphaMask, 0, size.cx * size.cy);

    Paint(CPoint(0, 0), CPoint(0, 0));

    int w = mOverlayWidth, h = mOverlayHeight;

    int x = (mOffsetX + 4) >> 3, y = (mOffsetY + 4) >> 3;
    int xo = 0, yo = 0;

    if(x < 0)
    {
        xo = -x;
        w -= -x;
        x = 0;
    }
    if(y < 0)
    {
        yo = -y;
        h -= -y;
        y = 0;
    }
    if(x + w > m_size.cx) w = m_size.cx - x;
    if(y + h > m_size.cy) h = m_size.cy - y;

    if(w <= 0 || h <= 0) return;

    const BYTE* src = mpOverlayBuffer + 2 * (mOverlayWidth * yo + xo);
    BYTE* dst = m_pAlphaMask + m_size.cx * y + x;

    while(h--)
    {
        for(ptrdiff_t wt = 0; wt < w; ++wt)
            dst[wt] = src[wt*2];

        src += 2 * mOverlayWidth;
        dst += m_size.cx;
    }

    if(inverse)
    {
        BYTE* dst = m_pAlphaMask;
        for(ptrdiff_t i = size.cx * size.cy; i > 0; --i, ++dst)
            *dst = 0x40 - *dst; // mask is 6 bit
    }
}

CClipper::~CClipper()
{
    if(m_pAlphaMask) delete [] m_pAlphaMask;
    m_pAlphaMask = NULL;
}

CWord* CClipper::Copy()
{
    return(DNew CClipper(m_str, m_size, m_scalex, m_scaley, m_inverse));
}

bool CClipper::Append(CWord* w)
{
    return(false);
}

// CLine

CLine::~CLine()
{
    POSITION pos = GetHeadPosition();
    while(pos) delete GetNext(pos);
}

void CLine::Compact()
{
    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);
        if(!w->m_fWhiteSpaceChar) break;

        m_width -= w->m_width;
        delete w;
        RemoveHead();
    }

    pos = GetTailPosition();
    while(pos)
    {
        CWord* w = GetPrev(pos);
        if(!w->m_fWhiteSpaceChar) break;

        m_width -= w->m_width;
        delete w;
        RemoveTail();
    }

    if(IsEmpty()) return;

    CLine l;
    l.AddTailList(this);
    RemoveAll();

    CWord* last = NULL;

    pos = l.GetHeadPosition();
    while(pos)
    {
        CWord* w = l.GetNext(pos);

        if(!last || !last->Append(w))
            AddTail(last = w->Copy());
    }

    m_ascent = m_descent = m_borderX = m_borderY = 0;

    pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(m_ascent < w->m_ascent) m_ascent = w->m_ascent;
        if(m_descent < w->m_descent) m_descent = w->m_descent;
        if(m_borderX < w->m_style.outlineWidthX) m_borderX = (int)(w->m_style.outlineWidthX + 0.5);
        if(m_borderY < w->m_style.outlineWidthY) m_borderY = (int)(w->m_style.outlineWidthY + 0.5);
    }
}

#ifdef _VSMOD // patch m006. moveable vector clip
CRect CLine::PaintShadow(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt)
#else
CRect CLine::PaintShadow(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha)
#endif
{
    CRect bbox(0, 0, 0, 0);

    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(w->m_fLineBreak) return(bbox); // should not happen since this class is just a line of text without any breaks

        if(w->m_style.shadowDepthX != 0 || w->m_style.shadowDepthY != 0)
        {
            int x = p.x + (int)(w->m_style.shadowDepthX + 0.5);
#ifdef _VSMOD // patch m001. Vertical fontspacing
            int y = p.y - w->m_style.mod_verticalSpace + m_ascent - w->m_ascent + (int)(w->m_style.shadowDepthY + 0.5);
#else
            int y = p.y + m_ascent - w->m_ascent + (int)(w->m_style.shadowDepthY + 0.5);
#endif
            DWORD a = 0xff - w->m_style.alpha[3];
            if(alpha > 0) a = MulDiv(a, 0xff - alpha, 0xff);
            COLORREF shadow = revcolor(w->m_style.colors[3]) | (a << 24);
            DWORD sw[6] = {shadow, 0xffffffff};

#ifdef _VSMOD // patch m011. jitter
            CPoint mod_jitter = w->m_style.mod_jitter.getOffset(rt);
            x += mod_jitter.x;
            y += mod_jitter.y;
            // patch m010. png background
            // subpixel positioning
            w->m_style.mod_grad.subpixx = x & 7;
            w->m_style.mod_grad.subpixy = y & 7;
            w->m_style.mod_grad.fadalpha = alpha;
#endif
            w->Paint(CPoint(x, y), org);

            if(w->m_style.borderStyle == 0)
            {
#ifdef _VSMOD // patch m004. gradient colors
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw,
                                w->m_ktype > 0 || w->m_style.alpha[0] < 0xff,
                                (w->m_style.outlineWidthX + w->m_style.outlineWidthY > 0) && !(w->m_ktype == 2 && time < w->m_kstart), 3, w->m_style.mod_grad, mod_vc);
#else
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw,
                                w->m_ktype > 0 || w->m_style.alpha[0] < 0xff,
                                (w->m_style.outlineWidthX + w->m_style.outlineWidthY > 0) && !(w->m_ktype == 2 && time < w->m_kstart));
#endif
            }
            else if(w->m_style.borderStyle == 1 && w->m_pOpaqueBox)
            {
#ifdef _VSMOD // patch m004. gradient colors
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false, 3, w->m_style.mod_grad, mod_vc);
#else
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false);
#endif
            }
        }

        p.x += w->m_width;
    }

    return(bbox);
}

#ifdef _VSMOD // patch m006. moveable vector clip
CRect CLine::PaintOutline(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt, const PaintPassFilter& filter)
#else
CRect CLine::PaintOutline(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha)
#endif
{
    CRect bbox(0, 0, 0, 0);

    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(w->m_fLineBreak) return(bbox); // should not happen since this class is just a line of text without any breaks

        const bool glowPass = filter.radius >= 0 && !filter.inner;
        if(filter.radius >= 0 && !WordMatchesFilter(w->m_style, filter))
        {
            p.x += w->m_width;
            continue;
        }

        if(w->m_style.outlineWidthX + w->m_style.outlineWidthY > 0 && !(w->m_ktype == 2 && time < w->m_kstart))
        {
            int x = p.x;
#ifdef _VSMOD // patch m001. Vertical fontspacing
            int y = p.y - w->m_style.mod_verticalSpace + m_ascent - w->m_ascent;
#else
            int y = p.y + m_ascent - w->m_ascent;
#endif
            DWORD aoutline = w->m_style.alpha[2];
            if(alpha > 0) aoutline += MulDiv(alpha, 0xff - w->m_style.alpha[2], 0xff);
            if(glowPass && w->m_style.mod_glowAlpha)
                aoutline += MulDiv(w->m_style.mod_glowAlpha, 0xff - aoutline, 0xff);
            if(aoutline > 0xff) aoutline = 0xff;
            /* In a glow pass the outline only supplies shape and emits in the
             * primary colour, unless \glowc overrides the emission. */
            const COLORREF emission = glowPass && w->m_style.mod_glowColorOverride
                                      ? w->m_style.mod_glowColor : w->m_style.colors[0];
            COLORREF outline = revcolor(glowPass ? emission : w->m_style.colors[2]) | ((0xff - aoutline) << 24);
            DWORD sw[6] = {outline, 0xffffffff};

#ifdef _VSMOD // patch m011. jitter
            CPoint mod_jitter = w->m_style.mod_jitter.getOffset(rt);
            x += mod_jitter.x;
            y += mod_jitter.y;
            // patch m010. png background
            // subpixel positioning
            w->m_style.mod_grad.subpixx = x & 7;
            w->m_style.mod_grad.subpixy = y & 7;
            w->m_style.mod_grad.fadalpha = alpha;
#endif

            w->Paint(CPoint(x, y), org);

            if(w->m_style.borderStyle == 0)
            {
#ifdef _VSMOD // patch m004. gradient colors
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw, !w->m_style.alpha[0] && !w->m_style.alpha[1] && !alpha && !w->m_style.mod_grad.bodyIsGradAlpha, true, 2, w->m_style.mod_grad, mod_vc);
#else
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw, !w->m_style.alpha[0] && !w->m_style.alpha[1] && !alpha, true);
#endif
            }
            else if(w->m_style.borderStyle == 1 && w->m_pOpaqueBox)
            {
#ifdef _VSMOD // patch m004. gradient colors
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false, 2, w->m_style.mod_grad, mod_vc);
#else
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false);
#endif
            }
        }

        p.x += w->m_width;
    }

    return(bbox);
}

#ifdef _VSMOD // patch m006. moveable vector clip
CRect CLine::PaintBody(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt, const PaintPassFilter& filter)
#else
CRect CLine::PaintBody(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha)
#endif
{
    CRect bbox(0, 0, 0, 0);

    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(w->m_fLineBreak) return(bbox); // should not happen since this class is just a line of text without any breaks

        const bool glowPass = filter.radius >= 0 && !filter.inner;
        if(filter.radius >= 0 && !WordMatchesFilter(w->m_style, filter))
        {
            p.x += w->m_width;
            continue;
        }

        int x = p.x;
#ifdef _VSMOD // patch m001. Vertical fontspacing
        int y = p.y - w->m_style.mod_verticalSpace + m_ascent - w->m_ascent;
#else
        int y = p.y + m_ascent - w->m_ascent;
#endif
        // colors

        DWORD aprimary = w->m_style.alpha[0];
        if(alpha > 0) aprimary += MulDiv(alpha, 0xff - w->m_style.alpha[0], 0xff);
        DWORD asecondary = w->m_style.alpha[1];
        if(alpha > 0) asecondary += MulDiv(alpha, 0xff - w->m_style.alpha[1], 0xff);
        if(glowPass && w->m_style.mod_glowAlpha)
        {
            aprimary += MulDiv(w->m_style.mod_glowAlpha, 0xff - aprimary, 0xff);
            asecondary += MulDiv(w->m_style.mod_glowAlpha, 0xff - asecondary, 0xff);
        }
        if(aprimary > 0xff) aprimary = 0xff;
        if(asecondary > 0xff) asecondary = 0xff;
        /* In a glow pass \glowc replaces the emission colour. */
        const COLORREF emissionPrimary = glowPass && w->m_style.mod_glowColorOverride
                                         ? w->m_style.mod_glowColor : w->m_style.colors[0];
        const COLORREF emissionSecondary = glowPass && w->m_style.mod_glowColorOverride
                                           ? w->m_style.mod_glowColor : w->m_style.colors[1];
        COLORREF primary = revcolor(emissionPrimary) | ((0xff - aprimary) << 24);
        COLORREF secondary = revcolor(emissionSecondary) | ((0xff - asecondary) << 24);

        DWORD sw[6] = {primary, 0, secondary};

        // karaoke

        double t;

        if(w->m_ktype == 0 || w->m_ktype == 2)
        {
            t = time < w->m_kstart ? 0 : 1;
        }
        else if(w->m_ktype == 1)
        {
            if(time < w->m_kstart) t = 0;
            else if(time < w->m_kend)
            {
                t = 1.0 * (time - w->m_kstart) / (w->m_kend - w->m_kstart);

                double angle = fmod(w->m_style.fontAngleZ, 360.0);
                if(angle > 90 && angle < 270)
                {
                    t = 1 - t;
                    COLORREF tmp = sw[0];
                    sw[0] = sw[2];
                    sw[2] = tmp;
                }
            }
            else t = 1.0;
        }

        if(t >= 1)
        {
            sw[1] = 0xFFFFFFF;
        }

        // move dividerpoint
        int bluradjust = 0;
        if(w->m_style.fGaussianBlur > 0)
            bluradjust += (int)(w->m_style.fGaussianBlur * 3 * 8 + 0.5) | 1;
        if(w->m_style.fBlur)
            bluradjust += 8;
        double tx = w->m_style.fontAngleZ;
        sw[4] = sw[2];
        sw[5] = 0x00ffffff;

#ifdef _VSMOD // patch m011. jitter
        CPoint mod_jitter = w->m_style.mod_jitter.getOffset(rt);
        x += mod_jitter.x;
        y += mod_jitter.y;
        // patch m010. png background
        // subpixel positioning
        w->m_style.mod_grad.subpixx = x & 7;
        w->m_style.mod_grad.subpixy = y & 7;
        w->m_style.mod_grad.fadalpha = alpha;
#endif

        w->Paint(CPoint(x, y), org);

        sw[3] = vsfm::CvtTruncSd(w->m_style.outlineWidthX + t * w->getOverlayWidth() + t * bluradjust) >> 3;

#ifdef _VSMOD // patch m004. gradient colors
        bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false, 0, w->m_style.mod_grad, mod_vc);
#else
        bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false);
#endif
        p.x += w->m_width;
    }

    return(bbox);
}


// CSubtitle

CSubtitle::CSubtitle()
{
    memset(m_effects, 0, sizeof(Effect*)*EF_NUMBEROFEFFECTS);
    m_pClipper = NULL;
    m_clipInverse = false;
#ifdef _VSMOD // patch m014. feathered clip
    m_clipFeather = 0;
    m_clipExplicit = false;
#endif
#ifdef _VSMOD // patch m016. track matte
    m_matteMode = 0;
    m_matteOffset = 1;
#endif
    m_scalex = m_scaley = 1;
}

CSubtitle::~CSubtitle()
{
    Empty();
}

void CSubtitle::Empty()
{
    POSITION pos = GetHeadPosition();
    while(pos) delete GetNext(pos);

    pos = m_words.GetHeadPosition();
    while(pos) delete m_words.GetNext(pos);

    for(ptrdiff_t i = 0; i < EF_NUMBEROFEFFECTS; i++)
    {
        if(m_effects[i]) delete m_effects[i];
    }
    memset(m_effects, 0, sizeof(Effect*)*EF_NUMBEROFEFFECTS);

    if(m_pClipper) delete m_pClipper;
    m_pClipper = NULL;
}

int CSubtitle::GetFullWidth()
{
    int width = 0;

    POSITION pos = m_words.GetHeadPosition();
    while(pos) width += m_words.GetNext(pos)->m_width;

    return(width);
}

int CSubtitle::GetFullLineWidth(POSITION pos)
{
    int width = 0;

    while(pos)
    {
        CWord* w = m_words.GetNext(pos);
        if(w->m_fLineBreak) break;
        width += w->m_width;
    }

    return(width);
}

int CSubtitle::GetWrapWidth(POSITION pos, int maxwidth)
{
    if(m_wrapStyle == 0 || m_wrapStyle == 3)
    {
        if(maxwidth > 0)
        {
//			int fullwidth = GetFullWidth();
            int fullwidth = GetFullLineWidth(pos);

            int minwidth = fullwidth / ((abs(fullwidth) / maxwidth) + 1);

            int width = 0, wordwidth = 0;

            while(pos && width < minwidth)
            {
                CWord* w = m_words.GetNext(pos);
                wordwidth = w->m_width;
                if(abs(width + wordwidth) < abs(maxwidth)) width += wordwidth;
            }

            if(m_wrapStyle == 3 && width < fullwidth && fullwidth - width + wordwidth < maxwidth) width -= wordwidth;
            maxwidth = width;
        }
    }
    else if(m_wrapStyle == 1)
    {
//		maxwidth = maxwidth;
    }
    else if(m_wrapStyle == 2)
    {
        maxwidth = INT_MAX;
    }

    return(maxwidth);
}

CLine* CSubtitle::GetNextLine(POSITION& pos, int maxwidth)
{
    if(pos == NULL) return(NULL);

    CLine* ret = DNew CLine();
    if(!ret) return(NULL);

    ret->m_width = ret->m_ascent = ret->m_descent = ret->m_borderX = ret->m_borderY = 0;

    maxwidth = GetWrapWidth(pos, maxwidth);

    bool fEmptyLine = true;

    while(pos)
    {
        CWord* w = m_words.GetNext(pos);

        if(ret->m_ascent < w->m_ascent) ret->m_ascent = w->m_ascent;
        if(ret->m_descent < w->m_descent) ret->m_descent = w->m_descent;
        if(ret->m_borderX < w->m_style.outlineWidthX) ret->m_borderX = (int)(w->m_style.outlineWidthX + 0.5);
        if(ret->m_borderY < w->m_style.outlineWidthY) ret->m_borderY = (int)(w->m_style.outlineWidthY + 0.5);

        if(w->m_fLineBreak)
        {
            if(fEmptyLine)
            {
                ret->m_ascent /= 2;
                ret->m_descent /= 2;
                ret->m_borderX = ret->m_borderY = 0;
            }

            ret->Compact();

            return(ret);
        }

        fEmptyLine = false;

        bool fWSC = w->m_fWhiteSpaceChar;

        int width = w->m_width;
        POSITION pos2 = pos;
        while(pos2)
        {
            if(m_words.GetAt(pos2)->m_fWhiteSpaceChar != fWSC
               || m_words.GetAt(pos2)->m_fLineBreak) break;

            CWord* w2 = m_words.GetNext(pos2);
            width += w2->m_width;
        }

        if((ret->m_width += width) <= maxwidth || ret->IsEmpty())
        {
            ret->AddTail(w->Copy());

            while(pos != pos2)
            {
                ret->AddTail(m_words.GetNext(pos)->Copy());
            }

            pos = pos2;
        }
        else
        {
            if(pos) m_words.GetPrev(pos);
            else pos = m_words.GetTailPosition();

            ret->m_width -= width;

            break;
        }
    }

    ret->Compact();

    return(ret);
}

void CSubtitle::CreateClippers(CSize size)
{
    size.cx >>= 3;
    size.cy >>= 3;

    if(m_effects[EF_BANNER] && m_effects[EF_BANNER]->param[2])
    {
        int width = m_effects[EF_BANNER]->param[2];

        int w = size.cx, h = size.cy;

        if(!m_pClipper)
        {
            CStringW str;
            str.Format(L"m %d %d l %d %d %d %d %d %d", 0, 0, w, 0, w, h, 0, h);
            m_pClipper = DNew CClipper(str, size, 1, 1, false);
            if(!m_pClipper) return;
        }

        int da = (64 << 8) / width;
        BYTE* am = m_pClipper->m_pAlphaMask;

        for(ptrdiff_t j = 0; j < h; j++, am += w)
        {
            int a = 0;
            int k = std::min(width, w);

            for(ptrdiff_t i = 0; i < k; i++, a += da)
                am[i] = (am[i] * a) >> 14;

            a = 0x40 << 8;
            k = w - width;

            if(k < 0)
            {
                a -= -k * da;
                k = 0;
            }

            for(ptrdiff_t i = k; i < w; i++, a -= da)
                am[i] = (am[i] * a) >> 14;
        }
    }
    else if(m_effects[EF_SCROLL] && m_effects[EF_SCROLL]->param[4])
    {
        int height = m_effects[EF_SCROLL]->param[4];

        int w = size.cx, h = size.cy;

        if(!m_pClipper)
        {
            CStringW str;
            str.Format(L"m %d %d l %d %d %d %d %d %d", 0, 0, w, 0, w, h, 0, h);
            m_pClipper = DNew CClipper(str, size, 1, 1, false);
            if(!m_pClipper) return;
        }

        int da = (64 << 8) / height;
        int a = 0;
        int k = m_effects[EF_SCROLL]->param[0] >> 3;
        int l = k + height;
        if(k < 0)
        {
            a += -k * da;
            k = 0;
        }
        if(l > h)
        {
            l = h;
        }

        if(k < h)
        {
            BYTE* am = &m_pClipper->m_pAlphaMask[k*w];

            memset(m_pClipper->m_pAlphaMask, 0, am - m_pClipper->m_pAlphaMask);

            for(ptrdiff_t j = k; j < l; j++, a += da)
            {
                for(ptrdiff_t i = 0; i < w; i++, am++)
                    *am = ((*am) * a) >> 14;
            }
        }

        da = -(64 << 8) / height;
        a = 0x40 << 8;
        l = m_effects[EF_SCROLL]->param[1] >> 3;
        k = l - height;
        if(k < 0)
        {
            a += -k * da;
            k = 0;
        }
        if(l > h)
        {
            l = h;
        }

        if(k < h)
        {
            BYTE* am = &m_pClipper->m_pAlphaMask[k*w];

            int j = k;
            for(; j < l; j++, a += da)
            {
                for(ptrdiff_t i = 0; i < w; i++, am++)
                    *am = ((*am) * a) >> 14;
            }

            memset(am, 0, (h - j)*w);
        }
    }
#ifdef _VSMOD // patch m014. feathered vector clip
    /* Feathering a vector clip blurs its coverage mask once, here where every
     * tag has been parsed regardless of their order in the line. Animated
     * subtitles are rebuilt every frame, so even an animated feather stays
     * correct. All mask consumers (text drawing, glow clipping) then see the
     * soft edge with no further changes. */
    if(m_clipFeather > 0 && m_pClipper && m_pClipper->m_pAlphaMask
       && m_pClipper->m_size.cx > 0 && m_pClipper->m_size.cy > 0)
    {
        FeatherVectorMaskBlur(m_pClipper->m_pAlphaMask, m_pClipper->m_size.cx, m_pClipper->m_size.cy,
                              (int)(m_clipFeather + 0.5));
    }
#endif

}

void CSubtitle::MakeLines(CSize size, CRect marginRect)
{
    CSize spaceNeeded(0, 0);

    bool fFirstLine = true;

    m_topborder = m_bottomborder = 0;

    CLine* l = NULL;

    POSITION pos = m_words.GetHeadPosition();
    while(pos)
    {
        l = GetNextLine(pos, size.cx - marginRect.left - marginRect.right);
        if(!l) break;

        if(fFirstLine)
        {
            m_topborder = l->m_borderY;
            fFirstLine = false;
        }

        spaceNeeded.cx = std::max<LONG>(l->m_width + l->m_borderX, spaceNeeded.cx);
        spaceNeeded.cy += l->m_ascent + l->m_descent;

        AddTail(l);
    }

    if(l) m_bottomborder = l->m_borderY;

    m_rect = CRect(
                 CPoint((m_scrAlignment % 3) == 1 ? marginRect.left
                        : (m_scrAlignment % 3) == 2 ? (marginRect.left + (size.cx - marginRect.right) - spaceNeeded.cx + 1) / 2
                        : (size.cx - marginRect.right - spaceNeeded.cx),
                        m_scrAlignment <= 3 ? (size.cy - marginRect.bottom - spaceNeeded.cy)
                        : m_scrAlignment <= 6 ? (marginRect.top + (size.cy - marginRect.bottom) - spaceNeeded.cy + 1) / 2
                        : marginRect.top),
                 spaceNeeded);
}

// CScreenLayoutAllocator

void CScreenLayoutAllocator::Empty()
{
    m_subrects.RemoveAll();
}

void CScreenLayoutAllocator::AdvanceToSegment(int segment, const CAtlArray<int>& sa)
{
    POSITION pos = m_subrects.GetHeadPosition();
    while(pos)
    {
        POSITION prev = pos;

        SubRect& sr = m_subrects.GetNext(pos);

        bool fFound = false;

        if(abs(sr.segment - segment) <= 1) // using abs() makes it possible to play the subs backwards, too :)
        {
            for(ptrdiff_t i = 0; i < sa.GetCount() && !fFound; i++)
            {
                if(sa[i] == sr.entry)
                {
                    sr.segment = segment;
                    fFound = true;
                }
            }
        }

        if(!fFound) m_subrects.RemoveAt(prev);
    }
}

CRect CScreenLayoutAllocator::AllocRect(CSubtitle* s, int segment, int entry, int layer, int collisions)
{
    // TODO: handle collisions == 1 (reversed collisions)

    POSITION pos = m_subrects.GetHeadPosition();
    while(pos)
    {
        SubRect& sr = m_subrects.GetNext(pos);
        if(sr.segment == segment && sr.entry == entry)
        {
            return(sr.r + CRect(0, -s->m_topborder, 0, -s->m_bottomborder));
        }
    }

    CRect r = s->m_rect + CRect(0, s->m_topborder, 0, s->m_bottomborder);

    bool fSearchDown = s->m_scrAlignment > 3;

    bool fOK;

    do
    {
        fOK = true;

        pos = m_subrects.GetHeadPosition();
        while(pos)
        {
            SubRect& sr = m_subrects.GetNext(pos);

            if(layer == sr.layer && !(r & sr.r).IsRectEmpty())
            {
                if(fSearchDown)
                {
                    r.bottom = sr.r.bottom + r.Height();
                    r.top = sr.r.bottom;
                }
                else
                {
                    r.top = sr.r.top - r.Height();
                    r.bottom = sr.r.top;
                }

                fOK = false;
            }
        }
    }
    while(!fOK);

    SubRect sr;
    sr.r = r;
    sr.segment = segment;
    sr.entry = entry;
    sr.layer = layer;
    m_subrects.AddTail(sr);

    return(sr.r + CRect(0, -s->m_topborder, 0, -s->m_bottomborder));
}

// CRenderedTextSubtitle

CRenderedTextSubtitle::CRenderedTextSubtitle(CCritSec* pLock, STSStyle *styleOverride, bool doOverride)
    : m_doOverrideStyle(doOverride), m_pStyleOverride(styleOverride)
{
    (void)pLock;   // was handed to ISubPicProviderImpl; callers still pass it
    m_size = CSize(0, 0);
}

CRenderedTextSubtitle::~CRenderedTextSubtitle()
{
    Deinit();
}

void CRenderedTextSubtitle::Copy(CSimpleTextSubtitle& sts)
{
    CSimpleTextSubtitle::Copy(sts);

    m_size = CSize(0, 0);

    if(CRenderedTextSubtitle* pRTS = dynamic_cast<CRenderedTextSubtitle*>(&sts))
    {
        m_size = pRTS->m_size;
    }
}

void CRenderedTextSubtitle::Empty()
{
    Deinit();

    CSimpleTextSubtitle::Empty();
}

void CRenderedTextSubtitle::OnChanged()
{
    CSimpleTextSubtitle::OnChanged();

    POSITION pos = m_subtitleCache.GetStartPosition();
    while(pos)
    {
        int i;
        CSubtitle* s;
        m_subtitleCache.GetNextAssoc(pos, i, s);
        delete s;
    }

    m_subtitleCache.RemoveAll();

    m_sla.Empty();
}

bool CRenderedTextSubtitle::Init(CSize size, CRect vidrect)
{
    Deinit();

    m_size = CSize(size.cx * 8, size.cy * 8);
    m_vidrect = CRect(vidrect.left * 8, vidrect.top * 8, vidrect.right * 8, vidrect.bottom * 8);

    m_sla.Empty();

    return(true);
}

void CRenderedTextSubtitle::Deinit()
{
    POSITION pos = m_subtitleCache.GetStartPosition();
    while(pos)
    {
        int i;
        CSubtitle* s;
        m_subtitleCache.GetNextAssoc(pos, i, s);
        delete s;
    }

    m_subtitleCache.RemoveAll();

    m_sla.Empty();

    m_size = CSize(0, 0);
    m_vidrect.SetRectEmpty();
}

void CRenderedTextSubtitle::ParseEffect(CSubtitle* sub, CString str)
{
    str.Trim();
    if(!sub || str.IsEmpty()) return;

    const TCHAR* s = _tcschr(str, ';');
    if(!s)
    {
        s = (LPTSTR)(LPCTSTR)str;
        s += str.GetLength() - 1;
    }
    s++;
    CString effect = str.Left(s - str);

    if(!effect.CompareNoCase(_T("Banner;")))
    {
        int delay, lefttoright = 0, fadeawaywidth = 0;
        if(_stscanf(s, _T("%d;%d;%d"), &delay, &lefttoright, &fadeawaywidth) < 1) return;

        Effect* e = DNew Effect;
        if(!e) return;

        sub->m_effects[e->type = EF_BANNER] = e;
        e->param[0] = (int)(std::max(1.0 * delay / sub->m_scalex, 1.0));
        e->param[1] = lefttoright;
        e->param[2] = (int)(sub->m_scalex * fadeawaywidth);

        sub->m_wrapStyle = 2;
    }
    else if(!effect.CompareNoCase(_T("Scroll up;")) || !effect.CompareNoCase(_T("Scroll down;")))
    {
        int top, bottom, delay, fadeawayheight = 0;
        if(_stscanf(s, _T("%d;%d;%d;%d"), &top, &bottom, &delay, &fadeawayheight) < 3) return;

        if(top > bottom)
        {
            int tmp = top;
            top = bottom;
            bottom = tmp;
        }

        Effect* e = DNew Effect;
        if(!e) return;

        sub->m_effects[e->type = EF_SCROLL] = e;
        e->param[0] = (int)(sub->m_scaley * top * 8);
        e->param[1] = (int)(sub->m_scaley * bottom * 8);
        e->param[2] = (int)(std::max(1.0 * delay / sub->m_scaley, 1.0));
        e->param[3] = (effect.GetLength() == 12);
        e->param[4] = (int)(sub->m_scaley * fadeawayheight);
    }
}

void CRenderedTextSubtitle::ParseString(CSubtitle* sub, CStringW str, STSStyle& style)
{
    if(!sub) return;

    str.Replace(L"\\N", L"\n");
    str.Replace(L"\\n", (sub->m_wrapStyle < 2 || sub->m_wrapStyle == 3) ? L" " : L"\n");
    str.Replace(L"\\h", L"\x00A0");

    for(size_t i = 0, j = 0, len = str.GetLength(); j <= len; j++)
    {
        WCHAR c = str[j];

        if(c != '\n' && c != ' ' && c != '\x00A0' && c != 0)
            continue;

        if(i < j)
        {
            if(CWord* w = DNew CText(style, str.Mid(i, j - i), m_ktype, m_kstart, m_kend, sub->m_scalex, sub->m_scaley))
            {
                sub->m_words.AddTail(w);
                m_kstart = m_kend;
            }
        }

        if(c == '\n')
        {
            if(CWord* w = DNew CText(style, CStringW(), m_ktype, m_kstart, m_kend, sub->m_scalex, sub->m_scaley))
            {
                sub->m_words.AddTail(w);
                m_kstart = m_kend;
            }
        }
        else if(c == ' ' || c == '\x00A0')
        {
            if(CWord* w = DNew CText(style, CStringW(c), m_ktype, m_kstart, m_kend, sub->m_scalex, sub->m_scaley))
            {
                sub->m_words.AddTail(w);
                m_kstart = m_kend;
            }
        }

        i = j + 1;
    }

    return;
}

void CRenderedTextSubtitle::ParsePolygon(CSubtitle* sub, CStringW str, STSStyle& style)
{
    if(!sub || !str.GetLength() || !m_nPolygon) return;

    /* \pN draws at 1/2^(N-1) scale. Upstream's 1 << (N - 1) is undefined for
     * N > 32 or N < 1, and the CPUs disagree: x86 shifts by the count modulo
     * 32, ARM by the full count (giving 0). x86's result, which is upstream's,
     * on every platform. */
    const int div = (int)(1u << ((unsigned)(m_nPolygon - 1) & 31));
    if(CWord* w = DNew CPolygon(style, str, m_ktype, m_kstart, m_kend, sub->m_scalex / div, sub->m_scaley / div, m_polygonBaselineOffset))
    {
        sub->m_words.AddTail(w);
        m_kstart = m_kend;
    }
}

bool CRenderedTextSubtitle::ParseSSATag(CSubtitle* sub, CStringW str, STSStyle& style, STSStyle& org, bool fAnimate)
{
    if(!sub) return(false);

    int nTags = 0, nUnrecognizedTags = 0;

    for(int i = 0, j; (j = str.Find('\\', i)) >= 0; i = j)
    {
        CStringW cmd;
        for(WCHAR c = str[++j]; c && c != '(' && c != '\\'; cmd += c, c = str[++j]);
        cmd.Trim();
        if(cmd.IsEmpty()) continue;

        CAtlArray<CStringW> params;

        if(str[j] == '(')
        {
            CStringW param;
            // complex tags search
            int br = 1; // 1 bracket
            for(WCHAR c = str[++j]; c && br > 0; param += c, c = str[++j])
            {
                if(c == '(') br++;
                if(c == ')') br--;
                if(br == 0) break;
            }
            param.Trim();

            while(!param.IsEmpty())
            {
                int i = param.Find(','), j = param.Find('\\');

                if(i >= 0 && (j < 0 || i < j))
                {
                    CStringW s = param.Left(i).Trim();
                    if(!s.IsEmpty()) params.Add(s);
                    param = i + 1 < param.GetLength() ? param.Mid(i + 1) : CStringW();
                }
                else
                {
                    param.Trim();
                    if(!param.IsEmpty()) params.Add(param);
                    param.Empty();
                }
            }
        }

        if(!cmd.Find(L"1c") || !cmd.Find(L"2c") || !cmd.Find(L"3c") || !cmd.Find(L"4c"))
            params.Add(cmd.Mid(2).Trim(L"&H")), cmd = cmd.Left(2);
        else if(!cmd.Find(L"1a") || !cmd.Find(L"2a") || !cmd.Find(L"3a") || !cmd.Find(L"4a"))
            params.Add(cmd.Mid(2).Trim(L"&H")), cmd = cmd.Left(2);
#ifdef _VSMOD // patch m010. png background
        else if(!cmd.Find(L"1img") || !cmd.Find(L"2img") || !cmd.Find(L"3img") || !cmd.Find(L"4img"))
            ;//params.Add(cmd.Mid(4)), cmd = cmd.Left(4);
#endif
#ifdef _VSMOD // patch m015. arbitrary-angle gradient
        else if(!cmd.Find(L"vgrad"))
        {
            if(cmd.GetLength() > 5)
            {
                const WCHAR suffixStart = cmd[5];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(5));
                    cmd = cmd.Left(5);
                }
            }
        }
#endif
#ifdef _VSMOD // patch m004. gradient colors
        else if(!cmd.Find(L"1vc") || !cmd.Find(L"2vc") || !cmd.Find(L"3vc") || !cmd.Find(L"4vc"))
            ;//params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"1va") || !cmd.Find(L"2va") || !cmd.Find(L"3va") || !cmd.Find(L"4va"))
            ;//params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
#endif
        else if(!cmd.Find(L"alpha"))
            params.Add(cmd.Mid(5).Trim(L"&H")), cmd = cmd.Left(5);
        else if(!cmd.Find(L"an"))
            params.Add(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"a"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"blur"))
            params.Add(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"bord"))
            params.Add(cmd.Mid(4)), cmd = cmd.Left(4);
#ifdef _VSMOD // patch m014. bevel: must precede the be/b prefix branches
        else if(!cmd.Find(L"bevel"))
        {
            if(cmd.GetLength() > 5)
            {
                const WCHAR suffixStart = cmd[5];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(5));
                    cmd = cmd.Left(5);
                }
            }
        }
#endif
        else if(!cmd.Find(L"be"))
            params.Add(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"b"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m014. feathered clip: must precede the clip prefix branch
        else if(!cmd.Find(L"clipf"))
        {
            if(cmd.GetLength() > 5)
            {
                const WCHAR suffixStart = cmd[5];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(5));
                    cmd = cmd.Left(5);
                }
            }
        }
#endif
        else if(!cmd.Find(L"clip"))
            ;
        else if(!cmd.Find(L"c"))
            params.Add(cmd.Mid(1).Trim(L"&H")), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m008. distort
        else if(!cmd.Find(L"distort"))
            ;
#endif
        else if(!cmd.Find(L"fade"))
            ;
        else if(!cmd.Find(L"fe"))
            params.Add(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"fn"))
            params.Add(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"frx") || !cmd.Find(L"fry") || !cmd.Find(L"frz"))
            params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
#ifdef _VSMOD // patch m007. symbol rotating
        else if(!cmd.Find(L"frs"))
            params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
#endif
        else if(!cmd.Find(L"fax") || !cmd.Find(L"fay"))
            params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"fr"))
            params.Add(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"fscx") || !cmd.Find(L"fscy"))
            params.Add(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"fsc"))
            params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"fsp"))
            params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
#ifdef _VSMOD// patch m001. Vertical fontspacing
        else if(!cmd.Find(L"fsvp"))
            params.Add(cmd.Mid(4)), cmd = cmd.Left(4);
#endif
        else if(!cmd.Find(L"fs"))
            params.Add(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"iclip"))
            ;
#ifdef _VSMOD // patch m014. inner glow family: must precede the i prefix branch;
               // mblur has no conflicting prefix and lives here for tidiness
        else if(!cmd.Find(L"iglowc"))
            params.Add(cmd.Mid(6).Trim(L"&H")), cmd = cmd.Left(6);
        else if(!cmd.Find(L"iglowa"))
            params.Add(cmd.Mid(6).Trim(L"&H")), cmd = cmd.Left(6);
        else if(!cmd.Find(L"iglow"))
        {
            if(cmd.GetLength() > 5)
            {
                const WCHAR suffixStart = cmd[5];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(5));
                    cmd = cmd.Left(5);
                }
            }
        }
        else if(!cmd.Find(L"mblur"))
        {
            if(cmd.GetLength() > 5)
            {
                const WCHAR suffixStart = cmd[5];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(5));
                    cmd = cmd.Left(5);
                }
            }
        }
        else if(!cmd.Find(L"imatte"))
        {
            /* integer layer-offset suffix, same convention as matte below */
            if(cmd.GetLength() > 6)
            {
                const WCHAR suffixStart = cmd[6];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '+' || suffixStart == '-')
                {
                    params.Add(cmd.Mid(6));
                    cmd = cmd.Left(6);
                }
            }
        }
#endif
#ifdef _VSMOD // patch m016. track matte: no conflicting prefix among the
               // m-tags (move/mover/mblur), placed near them for tidiness
        else if(!cmd.Find(L"matte"))
        {
            if(cmd.GetLength() > 5)
            {
                const WCHAR suffixStart = cmd[5];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '+' || suffixStart == '-')
                {
                    params.Add(cmd.Mid(5));
                    cmd = cmd.Left(5);
                }
            }
        }
#endif
        else if(!cmd.Find(L"i"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m014. glow colour/alpha: must precede the glow prefix branch
        else if(!cmd.Find(L"glowc"))
            params.Add(cmd.Mid(5).Trim(L"&H")), cmd = cmd.Left(5);
        else if(!cmd.Find(L"glowa"))
            params.Add(cmd.Mid(5).Trim(L"&H")), cmd = cmd.Left(5);
#endif
#ifdef _VSMOD // patch m012. glow
        else if(!cmd.Find(L"glow"))
        {
            if(cmd.GetLength() > 4)
            {
                const WCHAR suffixStart = cmd[4];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(4));
                    cmd = cmd.Left(4);
                }
            }
        }
#endif
#ifdef _VSMOD // patch m016. radial gradient
        /* Must precede the 
 prefix branch, which would otherwise eat the
         * tag as a style reset. */
        else if(!cmd.Find(L"rgrad"))
        {
            if(cmd.GetLength() > 5)
            {
                const WCHAR suffixStart = cmd[5];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(5));
                    cmd = cmd.Left(5);
                }
            }
        }
#endif
#ifdef _VSMOD // patch m013. RGB split
        /* Must precede the 
 prefix branch, which would otherwise eat the
         * tag as a style reset. */
        else if(!cmd.Find(L"rgb"))
        {
            if(cmd.GetLength() > 3)
            {
                const WCHAR suffixStart = cmd[3];
                if((suffixStart >= '0' && suffixStart <= '9') || suffixStart == '-' || suffixStart == '+' || suffixStart == '.')
                {
                    params.Add(cmd.Mid(3));
                    cmd = cmd.Left(3);
                }
            }
        }
#endif
#ifdef _VSMOD // patch m011. jitter
        else if(!cmd.Find(L"jitter"))
            ;
#endif
        else if(!cmd.Find(L"kt") || !cmd.Find(L"kf") || !cmd.Find(L"ko"))
            params.Add(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"k") || !cmd.Find(L"K"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m005. add some move types
        else if(!cmd.Find(L"mover"))	// radial move
            ;
        else if(!cmd.Find(L"moves3"))	// square spline
            ;
        else if(!cmd.Find(L"moves4"))	// cubic spline
            ;
#endif
#ifdef _VSMOD // patch m006. moveable vector clip
        else if(!cmd.Find(L"movevc"))
            ;
#endif
        else if(!cmd.Find(L"move"))
            ;
        else if(!cmd.Find(L"org"))
            ;
        else if(!cmd.Find(L"pbo"))
            params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"pos"))
            ;
        else if(!cmd.Find(L"p"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"q"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m003. random text points
        else if(!cmd.Find(L"rndx") || !cmd.Find(L"rndy") || !cmd.Find(L"rndz") || !cmd.Find(L"rnds"))
            params.Add(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"rnd"))
            params.Add(cmd.Mid(3)), cmd = cmd.Left(3);
#endif
        else if(!cmd.Find(L"r"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"shad"))
            params.Add(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"s"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"t"))
            ;
        else if(!cmd.Find(L"u"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"xbord"))
            params.Add(cmd.Mid(5)), cmd = cmd.Left(5);
        else if(!cmd.Find(L"xshad"))
            params.Add(cmd.Mid(5)), cmd = cmd.Left(5);
        else if(!cmd.Find(L"ybord"))
            params.Add(cmd.Mid(5)), cmd = cmd.Left(5);
        else if(!cmd.Find(L"yshad"))
            params.Add(cmd.Mid(5)), cmd = cmd.Left(5);
#ifdef _VSMOD // patch m002. Z-coord
        else if(!cmd.Find(L"z"))
            params.Add(cmd.Mid(1)), cmd = cmd.Left(1);
#endif
        else
            nUnrecognizedTags++;

        nTags++;

        // TODO: call ParseStyleModifier(cmd, params, ..) and move the rest there

        CStringW p = params.GetCount() > 0 ? params[0] : CStringW();

        if(cmd == "1c" || cmd == L"2c" || cmd == L"3c" || cmd == L"4c")
        {
            int i = cmd[0] - '1';

            DWORD c = vsfm::WcsToL(p, NULL, 16);
            style.colors[i] = !p.IsEmpty()
                              ? (((int)CalcAnimation(c & 0xff, style.colors[i] & 0xff, fAnimate)) & 0xff
                                 | ((int)CalcAnimation(c & 0xff00, style.colors[i] & 0xff00, fAnimate)) & 0xff00
                                 | ((int)CalcAnimation(c & 0xff0000, style.colors[i] & 0xff0000, fAnimate)) & 0xff0000)
                              : org.colors[i];
#ifdef _VSMOD // patch m004. gradient colors
            style.mod_grad.colors[i] = style.colors[i];
            if(!fAnimate)
            {
                style.mod_grad.mode[i] = 0;
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[i][j] = !p.IsEmpty()
                                                 ? revcolor(c & 0xffffff)
                                                 : org.mod_grad.color[i][j];
                }
            }
            else if(style.mod_grad.mode[i] != 0)
            {
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[i][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation(c & 0xff, style.mod_grad.color[i][j] & 0xff, fAnimate)) & 0xff
                                                    | ((int)CalcAnimation(c & 0xff00, style.mod_grad.color[i][j] & 0xff00, fAnimate)) & 0xff00
                                                    | ((int)CalcAnimation(c & 0xff0000, style.mod_grad.color[i][j] & 0xff0000, fAnimate)) & 0xff0000)
                                                 : org.colors[i];
                }
            }
#endif
        }
        else if(cmd == L"1a" || cmd == L"2a" || cmd == L"3a" || cmd == L"4a")
        {
            DWORD al = vsfm::WcsToL(p, NULL, 16) & 0xff;
            int i = cmd[0] - '1';

            style.alpha[i] = !p.IsEmpty()
                             ? (BYTE)CalcAnimation(al, style.alpha[i], fAnimate)
                             : org.alpha[i];
#ifdef _VSMOD // patch m004. gradient colors
            style.mod_grad.alphas[i] = style.alpha[i];
            style.mod_grad.b_images[i].alpha = 255 - style.alpha[i];
            if(!fAnimate)
            {
                //style.mod_grad.mode[i] = 0;
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                 ? al
                                                 : org.mod_grad.alpha[i][j];
                }
            }
            else if(style.mod_grad.mode[i] != 0)
            {
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation(al, style.mod_grad.alpha[i][j], fAnimate)))
                                                 : org.alpha[i];
                }
            }
#endif
        }
#ifdef _VSMOD // patch m010. png background
        else if(cmd == L"1img" || cmd == L"2img" || cmd == L"3img" || cmd == L"4img")
        {
            int i = cmd[0] - '1';

            if(params.GetCount() >= 1)// file[,xoffset,yoffset[,angle]]
            {
                if(!fAnimate)
                {
                    CString fpath = !m_resPath.IsEmpty() ? m_resPath : m_path.Left(m_path.ReverseFind('\\') + 1);

                    bool t_init = false;
                    // buffer
                    for(ptrdiff_t k = 0, j = mod_images.GetCount(); k < j; k++)
                    {
                        MOD_PNGIMAGE t_temp = mod_images[k];
                        if(t_temp.filename == params[0]) // found buffered image
                        {
                            style.mod_grad.b_images[i] = t_temp;
                            t_init = true;
                            break;
                        }
                        if(t_temp.filename == fpath + params[0]) // found buffered image
                        {
                            style.mod_grad.b_images[i] = t_temp;
                            t_init = true;
                            break;
                        }
                    }
                    if(t_init)
                    {
                        style.mod_grad.mode[i] = 2;
                    }
                    else
                    {
                        // not found
                        MOD_PNGIMAGE t_temp;
                        if(t_temp.initImage(params[0])) // absolute path or default directory
                        {
                            style.mod_grad.mode[i] = 2;
                            style.mod_grad.b_images[i] = t_temp;
                            mod_images.Add(t_temp);
                        }
                        else
                        if(t_temp.initImage(fpath + params[0])) // path + relative path
                        {
                            style.mod_grad.mode[i] = 2;
                            style.mod_grad.b_images[i] = t_temp;
                            mod_images.Add(t_temp);
                        }
                    }
                }
                if(params.GetCount() >= 3)
                {
                    style.mod_grad.b_images[i].xoffset = !p.IsEmpty()
                                                         ? CalcAnimation(vsfm::WcsToL(params[1], NULL, 10), style.mod_grad.b_images[i].xoffset, fAnimate)
                                                         : org.mod_grad.b_images[i].xoffset;
                    style.mod_grad.b_images[i].yoffset = !p.IsEmpty()
                                                         ? CalcAnimation(vsfm::WcsToL(params[2], NULL, 10), style.mod_grad.b_images[i].yoffset, fAnimate)
                                                         : org.mod_grad.b_images[i].yoffset;
                }
            }
        }
#endif
#ifdef _VSMOD // patch m004. gradient colors
        else if(cmd == L"vgrad")
        {
            if(params.GetCount() > 0)
            {
                const double ang = vsfm::WcsToD(params[0], NULL);
                const double na = CalcAnimation(ang, style.mod_grad.angle, fAnimate);
                style.mod_grad.angle = na;
                style.mod_grad.angleEnabled = true;
                // patch m016. radial gradient: the two gradient modes are
                // mutually exclusive, the later tag wins.
                style.mod_grad.radialEnabled = false;
                const double rad = na * 3.14159265358979323846 / 180.0;
                style.mod_grad.angleCos = vsfm::DetCos(rad);
                style.mod_grad.angleSin = vsfm::DetSin(rad);
            }
            else
            {
                style.mod_grad.angle = org.mod_grad.angle;
                style.mod_grad.angleEnabled = org.mod_grad.angleEnabled;
                style.mod_grad.angleCos = org.mod_grad.angleCos;
                style.mod_grad.angleSin = org.mod_grad.angleSin;
            }
        }
#endif
#ifdef _VSMOD // patch m016. radial gradient
        else if(cmd == L"rgrad")
        {
            if(params.GetCount() > 0)
            {
                const double scale = vsfm::WcsToD(params[0], NULL);
                const double cx = params.GetCount() > 1 ? vsfm::WcsToD(params[1], NULL) : 0.5;
                const double cy = params.GetCount() > 2 ? vsfm::WcsToD(params[2], NULL) : 0.5;
                const double ns = CalcAnimation(scale, style.mod_grad.radialScale, fAnimate);
                style.mod_grad.radialScale = !(ns > 0) ? 0 : ns > 64 ? 64 : ns;
                style.mod_grad.radialCx = CalcAnimation(cx, style.mod_grad.radialCx, fAnimate);
                style.mod_grad.radialCy = CalcAnimation(cy, style.mod_grad.radialCy, fAnimate);
                style.mod_grad.radialEnabled = style.mod_grad.radialScale > 0;
                // the two gradient modes are mutually exclusive, the later
                // tag wins.
                if(style.mod_grad.radialEnabled)
                    style.mod_grad.angleEnabled = false;
            }
            else
            {
                style.mod_grad.radialScale = org.mod_grad.radialScale;
                style.mod_grad.radialCx = org.mod_grad.radialCx;
                style.mod_grad.radialCy = org.mod_grad.radialCy;
                style.mod_grad.radialEnabled = org.mod_grad.radialEnabled;
            }
        }
#endif
#ifdef _VSMOD // patch m004. gradient colors
        else if(cmd == L"1vc" || cmd == L"2vc" || cmd == L"3vc" || cmd == L"4vc")
        {
            int i = cmd[0] - '1';

            if(params.GetCount() >= 4)
            {
                DWORD c;
                for(int j = 0; j < 4; j++)
                {
                    c = vsfm::WcsToL(params[j].Trim(L"&H"), NULL, 16);
                    style.mod_grad.color[i][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation((c & 0xff0000) >> 16, style.mod_grad.color[i][j] & 0xff, fAnimate)) & 0xff
                                                    | ((int)CalcAnimation(c & 0xff00, style.mod_grad.color[i][j] & 0xff00, fAnimate)) & 0xff00
                                                    | ((int)CalcAnimation((c & 0xff) << 16, style.mod_grad.color[i][j] & 0xff0000, fAnimate)) & 0xff0000)
                                                 : org.mod_grad.color[i][j];
                }
                if (style.mod_grad.mode[i] == 0)
                {
                    for (int j = 0; j < 4; j++)
                    {
                        style.mod_grad.alpha[i][j] = style.alpha[i];
                    }
                }
                //if (!fAnimate)
                style.mod_grad.mode[i] = 1;

                if (i == 0 || i == 1)
                    style.mod_grad.mode[0] = style.mod_grad.mode[1] = 1;
            }
        }
        else if(cmd == L"1va" || cmd == L"2va" || cmd == L"3va" || cmd == L"4va")
        {
            int i = cmd[0] - '1';

            if(params.GetCount() >= 4)
            {
                int a;
                for(int j = 0; j < 4; j++)
                {
                    a = vsfm::WcsToL(params[j].Trim(L"&H"), NULL, 16);
                    style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                 ? (int)CalcAnimation(a, style.mod_grad.alpha[i][j], fAnimate) : org.mod_grad.alpha[i][j];
                }
                if(style.mod_grad.mode[i] == 0)
                {
                    for(int j = 0; j < 4; j++)
                    {
                        style.mod_grad.color[i][j] = revcolor(style.colors[i]);
                    }
                }
                //if (!fAnimate)
                style.mod_grad.mode[i] = 1;

                if (i == 0 || i == 1) {
                    style.mod_grad.mode[0] = style.mod_grad.mode[1] = 1;
                    style.mod_grad.bodyIsGradAlpha = true;
                }
            }
        }
#endif
        else if(cmd == L"alpha")
        {
            for(ptrdiff_t i = 0; i < 4; i++)
            {
                DWORD al = vsfm::WcsToL(p, NULL, 16) & 0xff;
                style.alpha[i] = !p.IsEmpty()
                                 ? (BYTE)CalcAnimation(al, style.alpha[i], fAnimate)
                                 : org.alpha[i];
#ifdef _VSMOD // patch m004. gradient colors
                style.mod_grad.alphas[i] = style.alpha[i];
                if(!fAnimate)
                {
                    //style.mod_grad.mode[i] = 0;
                    for(int j = 0; j < 4; j++)
                    {
                        style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                     ? al
                                                     : org.mod_grad.alpha[i][j];
                        style.mod_grad.b_images[i].alpha = 255 - al;
                    }
                }
                else if(style.mod_grad.mode[i] != 0)
                {
                    for(int j = 0; j < 4; j++)
                    {
                        style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                     ? (((int)CalcAnimation(al, style.mod_grad.alpha[i][j], fAnimate)))
                                                     : org.alpha[i];
                        style.mod_grad.b_images[i].alpha = 255 - style.alpha[i];
                    }
                }
#endif
            }
        }
        else if(cmd == L"an")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            if(sub->m_scrAlignment < 0)
                sub->m_scrAlignment = (n > 0 && n < 10) ? n : org.scrAlignment;
        }
        else if(cmd == L"a")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            if(sub->m_scrAlignment < 0)
                sub->m_scrAlignment = (n > 0 && n < 12) ? ((((n - 1) & 3) + 1) + ((n & 4) ? 6 : 0) + ((n & 8) ? 3 : 0)) : org.scrAlignment;
        }
        else if(cmd == L"blur")
        {
            double n = CalcAnimation(vsfm::WcsToD(p, NULL), style.fGaussianBlur, fAnimate);
            style.fGaussianBlur = !p.IsEmpty()
                                  ? (n < 0 ? 0 : n)
                                      : org.fGaussianBlur;
        }
        else if(cmd == L"bord")
        {
            double dst = vsfm::WcsToD(p, NULL);
            double nx = CalcAnimation(dst, style.outlineWidthX, fAnimate);
            style.outlineWidthX = !p.IsEmpty()
                                  ? (nx < 0 ? 0 : nx)
                                      : org.outlineWidthX;
            double ny = CalcAnimation(dst, style.outlineWidthY, fAnimate);
            style.outlineWidthY = !p.IsEmpty()
                                  ? (ny < 0 ? 0 : ny)
                                      : org.outlineWidthY;
        }
        else if(cmd == L"be")
        {
            int n = (int)(CalcAnimation(vsfm::WcsToL(p, NULL, 10), style.fBlur, fAnimate) + 0.5);
            style.fBlur = !p.IsEmpty()
                          ? n
                          : org.fBlur;
        }
        else if(cmd == L"b")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            style.fontWeight = !p.IsEmpty()
                               ? (n == 0 ? FW_NORMAL : n == 1 ? FW_BOLD : n >= 100 ? n : org.fontWeight)
                                   : org.fontWeight;
        }
        else if(cmd == L"clip" || cmd == L"iclip")
        {
            bool invert = (cmd == L"iclip");

            if(params.GetCount() == 1 && !sub->m_pClipper)
            {
                sub->m_pClipper = DNew CClipper(params[0], CSize(m_size.cx >> 3, m_size.cy >> 3), sub->m_scalex, sub->m_scaley, invert);
            }
            else if(params.GetCount() == 2 && !sub->m_pClipper)
            {
                int scale = std::max<long>(vsfm::WcsToL(p, NULL, 10), 1);
                sub->m_pClipper = DNew CClipper(params[1], CSize(m_size.cx >> 3, m_size.cy >> 3), sub->m_scalex / (int)(1u << ((unsigned)(scale - 1) & 31)), sub->m_scaley / (int)(1u << ((unsigned)(scale - 1) & 31)), invert);  // see ParsePolygon
            }
            else if(params.GetCount() == 4)
            {
                CRect r;

                sub->m_clipInverse = invert;

                r.SetRect(
                    vsfm::WcsToL(params[0], NULL, 10),
                    vsfm::WcsToL(params[1], NULL, 10),
                    vsfm::WcsToL(params[2], NULL, 10),
                    vsfm::WcsToL(params[3], NULL, 10));

                CPoint o(0, 0);

                if(sub->m_relativeTo == 1) // TODO: this should also apply to the other two clippings above
                {
                    o.x = m_vidrect.left >> 3;
                    o.y = m_vidrect.top >> 3;
                }

                sub->m_clip.SetRect(
                    (int)CalcAnimation(sub->m_scalex * r.left + o.x, sub->m_clip.left, fAnimate),
                    (int)CalcAnimation(sub->m_scaley * r.top + o.y, sub->m_clip.top, fAnimate),
                    (int)CalcAnimation(sub->m_scalex * r.right + o.x, sub->m_clip.right, fAnimate),
                    (int)CalcAnimation(sub->m_scaley * r.bottom + o.y, sub->m_clip.bottom, fAnimate));
#ifdef _VSMOD // patch m014. feathered clip
                sub->m_clipExplicit = true;
#endif
            }
        }
#ifdef _VSMOD // patch m016. track matte
        /* Event-level visibility flag, not a style property: the tag marks the
         * whole event as a hidden alpha matte whose silhouette masks events on
         * layer (matte layer + offset). No animation support. */
        else if(cmd == L"matte" || cmd == L"imatte")
        {
            const int mode = (cmd == L"imatte") ? 2 : 1;
            if(params.GetCount() > 0)
            {
                int n = vsfm::WcsToL(params[0], NULL, 10);
                if(n > 64) n = 64;
                if(n <= 0)
                {
                    sub->m_matteMode = 0;
                    sub->m_matteOffset = 1;
                }
                else
                {
                    sub->m_matteMode = mode;
                    sub->m_matteOffset = n;
                }
            }
            else
            {
                sub->m_matteMode = mode;
                sub->m_matteOffset = 1;
            }
        }
#endif
        else if(cmd == L"c")
        {
            DWORD c = vsfm::WcsToL(p, NULL, 16);
            style.colors[0] = !p.IsEmpty()
                              ? (((int)CalcAnimation(c & 0xff, style.colors[0] & 0xff, fAnimate)) & 0xff
                                 | ((int)CalcAnimation(c & 0xff00, style.colors[0] & 0xff00, fAnimate)) & 0xff00
                                 | ((int)CalcAnimation(c & 0xff0000, style.colors[0] & 0xff0000, fAnimate)) & 0xff0000)
                              : org.colors[0];
#ifdef _VSMOD // patch m004. gradient colors
            style.mod_grad.colors[0] = style.colors[0];
            if(!fAnimate)
            {
                style.mod_grad.mode[0] = 0;
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[0][j] = !p.IsEmpty()
                                                 ? ((int)c & 0xff
                                                    | (int)c & 0xff00
                                                    | (int)c & 0xff0000)
                                                 : org.mod_grad.color[0][j];
                }
            }
            else if(style.mod_grad.mode[0] != 0)
            {
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[0][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation(c & 0xff, style.mod_grad.color[0][j] & 0xff, fAnimate)) & 0xff
                                                    | ((int)CalcAnimation(c & 0xff00, style.mod_grad.color[0][j] & 0xff00, fAnimate)) & 0xff00
                                                    | ((int)CalcAnimation(c & 0xff0000, style.mod_grad.color[0][j] & 0xff0000, fAnimate)) & 0xff0000)
                                                 : org.colors[0];
                }
            }
#endif
        }
#ifdef _VSMOD // patch m008. distort
        else if(cmd == L"distort")
        {
            if(params.GetCount() >= 6)
            {
                DWORD c;
                for(int j = 0; j < 3; j++)
                {
                    style.mod_distort.pointsx[j] = !p.IsEmpty()
                                                   ? (CalcAnimation(vsfm::WcsToD(params[j*2], NULL), style.mod_distort.pointsx[j], fAnimate))
                                                   : org.mod_distort.pointsx[j];
                    style.mod_distort.pointsy[j] = !p.IsEmpty()
                                                   ? (CalcAnimation(vsfm::WcsToD(params[j*2+1], NULL), style.mod_distort.pointsy[j], fAnimate))
                                                   : org.mod_distort.pointsy[j];
                }
                style.mod_distort.enabled = true;
            }
        }
#endif
        else if(cmd == L"fade" || cmd == L"fad")
        {
            if(params.GetCount() == 7 && !sub->m_effects[EF_FADE])// {\fade(a1=param[0], a2=param[1], a3=param[2], t1=t[0], t2=t[1], t3=t[2], t4=t[3])
            {
                if(Effect* e = DNew Effect)
                {
                    for(ptrdiff_t i = 0; i < 3; i++)
                        e->param[i] = vsfm::WcsToL(params[i], NULL, 10);
                    for(ptrdiff_t i = 0; i < 4; i++)
                        e->t[i] = vsfm::WcsToL(params[3+i], NULL, 10);

                    sub->m_effects[EF_FADE] = e;
                }
            }
            else if(params.GetCount() == 2 && !sub->m_effects[EF_FADE]) // {\fad(t1=t[1], t2=t[2])
            {
                if(Effect* e = DNew Effect)
                {
                    e->param[0] = e->param[2] = 0xff;
                    e->param[1] = 0x00;
                    for(ptrdiff_t i = 1; i < 3; i++)
                        e->t[i] = vsfm::WcsToL(params[i-1], NULL, 10);
                    e->t[0] = e->t[3] = -1; // will be substituted with "start" and "end"

                    sub->m_effects[EF_FADE] = e;
                }
            }
        }
        else if(cmd == L"fax")
        {
            style.fontShiftX = !p.IsEmpty()
                               ? CalcAnimation(vsfm::WcsToD(p, NULL), style.fontShiftX, fAnimate)
                               : org.fontShiftX;
        }
        else if(cmd == L"fay")
        {
            style.fontShiftY = !p.IsEmpty()
                               ? CalcAnimation(vsfm::WcsToD(p, NULL), style.fontShiftY, fAnimate)
                               : org.fontShiftY;
        }
        else if(cmd == L"fe")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            style.charSet = !p.IsEmpty()
                            ? n
                            : org.charSet;
        }
        else if(cmd == L"fn")
        {
            style.fontName = (!p.IsEmpty() && p != '0')
                             ? CString(p).Trim()
                             : org.fontName;
        }
#ifdef _VSMOD // patch m007. symbol rotating
        else if(cmd == L"frs")
        {
            double dst = vsfm::WcsToD(p, NULL) * 10;

            style.mod_fontOrient = !p.IsEmpty()
                                   ? CalcAnimation(dst, style.mod_fontOrient, fAnimate)
                                   : org.mod_fontOrient;
        }
#endif
        else if(cmd == L"frx")
        {
            style.fontAngleX = !p.IsEmpty()
                               ? CalcAnimation(vsfm::WcsToD(p, NULL), style.fontAngleX, fAnimate)
                               : org.fontAngleX;
        }
        else if(cmd == L"fry")
        {
            style.fontAngleY = !p.IsEmpty()
                               ? CalcAnimation(vsfm::WcsToD(p, NULL), style.fontAngleY, fAnimate)
                               : org.fontAngleY;
        }
        else if(cmd == L"frz" || cmd == L"fr")
        {
            style.fontAngleZ = !p.IsEmpty()
                               ? CalcAnimation(vsfm::WcsToD(p, NULL), style.fontAngleZ, fAnimate)
                               : org.fontAngleZ;
        }
        else if(cmd == L"fscx")
        {
            double n = CalcAnimation(vsfm::WcsToL(p, NULL, 10), style.fontScaleX, fAnimate);
            style.fontScaleX = !p.IsEmpty()
                               ? ((n < 0) ? 0 : n)
                                   : org.fontScaleX;
        }
        else if(cmd == L"fscy")
        {
            double n = CalcAnimation(vsfm::WcsToL(p, NULL, 10), style.fontScaleY, fAnimate);
            style.fontScaleY = !p.IsEmpty()
                               ? ((n < 0) ? 0 : n)
                                   : org.fontScaleY;
        }
        else if(cmd == L"fsc")
        {
#ifdef _VSMOD // patch f004. \fsc(%f) is working
            double dst = vsfm::WcsToD(p, NULL);
            double nx = CalcAnimation(dst, style.fontScaleX, fAnimate);
            style.fontScaleX = !p.IsEmpty()
                               ? (nx < 0 ? 0 : nx)
                                   : org.fontScaleX;
            double ny = CalcAnimation(dst, style.fontScaleY, fAnimate);
            style.fontScaleY = !p.IsEmpty()
                               ? (ny < 0 ? 0 : ny)
                                   : org.fontScaleY;
#else
            style.fontScaleX = org.fontScaleX;
            style.fontScaleY = org.fontScaleY;
#endif
        }
        else if(cmd == L"fsp")
        {
            style.fontSpacing = !p.IsEmpty()
                                ? CalcAnimation(vsfm::WcsToD(p, NULL), style.fontSpacing, fAnimate)
                                : org.fontSpacing;
        }
#ifdef _VSMOD // patch m001. Vertical fontspacing
        else if(cmd == L"fsvp")
        {
            double dst = vsfm::WcsToD(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_verticalSpace, fAnimate);
            style.mod_verticalSpace = !p.IsEmpty() ? nx : org.mod_verticalSpace;
        }
#endif
        else if(cmd == L"fs")
        {
            if(!p.IsEmpty())
            {
                if(p[0] == '-' || p[0] == '+')
                {
                    double n = CalcAnimation(style.fontSize + style.fontSize * vsfm::WcsToL(p, NULL, 10) / 10, style.fontSize, fAnimate);
                    style.fontSize = (n > 0) ? n : org.fontSize;
                }
                else
                {
                    double n = CalcAnimation(vsfm::WcsToL(p, NULL, 10), style.fontSize, fAnimate);
                    style.fontSize = (n > 0) ? n : org.fontSize;
                }
            }
            else
            {
                style.fontSize = org.fontSize;
            }
        }
        else if(cmd == L"i")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            style.fItalic = !p.IsEmpty()
                            ? (n == 0 ? false : n == 1 ? true : org.fItalic)
                                : org.fItalic;
        }
#ifdef _VSMOD // patch m012. glow
        else if(cmd == L"glow")
        {
            if(params.GetCount() > 0)
            {
                const double radius = vsfm::WcsToD(params[0], NULL);
                const double strength = params.GetCount() > 1 ? vsfm::WcsToD(params[1], NULL) : 1.0;
                const double nr = CalcAnimation(radius, style.mod_glowRadius, fAnimate);
                const double ns = CalcAnimation(strength, style.mod_glowStrength, fAnimate);
                style.mod_glowRadius = !(nr > 0) ? 0 : nr > 4096 ? 4096 : nr;
                style.mod_glowStrength = !(ns > 0) ? 0 : ns > 64 ? 64 : ns;
            }
            else
            {
                style.mod_glowRadius = org.mod_glowRadius;
                style.mod_glowStrength = org.mod_glowStrength;
            }
        }
        else if(cmd == L"rgb")
        {
            if(params.GetCount() > 0)
            {
                const double dist = vsfm::WcsToD(params[0], NULL);
                const double nd = CalcAnimation(dist, style.mod_rgbSplit, fAnimate);
                style.mod_rgbSplit = nd > 4096 ? 4096 : nd < -4096 ? -4096 : nd;
            }
            else
            {
                style.mod_rgbSplit = org.mod_rgbSplit;
            }
        }
        else if(cmd == L"glowc" || cmd == L"iglowc")
        {
            COLORREF* target = cmd == L"glowc" ? &style.mod_glowColor : &style.mod_iglowColor;
            bool* overrideFlag = cmd == L"glowc" ? &style.mod_glowColorOverride : &style.mod_iglowColorOverride;
            const COLORREF orgColor = cmd == L"glowc" ? org.mod_glowColor : org.mod_iglowColor;
            const bool orgOverride = cmd == L"glowc" ? org.mod_glowColorOverride : org.mod_iglowColorOverride;

            CStringW hexp = p.Trim(L"&H");
            if(!hexp.IsEmpty())
            {
                DWORD c = vsfm::WcsToL(hexp, NULL, 16);
                *target = (((int)CalcAnimation(c & 0xff, *target & 0xff, fAnimate)) & 0xff)
                          | (((int)CalcAnimation(c & 0xff00, *target & 0xff00, fAnimate)) & 0xff00)
                          | (((int)CalcAnimation(c & 0xff0000, *target & 0xff0000, fAnimate)) & 0xff0000);
                *overrideFlag = true;
            }
            else
            {
                *target = orgColor;
                *overrideFlag = orgOverride;
            }
        }
        else if(cmd == L"glowa" || cmd == L"iglowa")
        {
            BYTE* target = cmd == L"glowa" ? &style.mod_glowAlpha : &style.mod_iglowAlpha;
            const BYTE orgAlpha = cmd == L"glowa" ? org.mod_glowAlpha : org.mod_iglowAlpha;

            CStringW hexp = p.Trim(L"&H");
            if(!hexp.IsEmpty())
            {
                DWORD al = vsfm::WcsToL(hexp, NULL, 16) & 0xff;
                *target = (BYTE)CalcAnimation(al, *target, fAnimate);
            }
            else
            {
                *target = orgAlpha;
            }
        }
        else if(cmd == L"iglow")
        {
            if(params.GetCount() > 0)
            {
                const double radius = vsfm::WcsToD(params[0], NULL);
                const double strength = params.GetCount() > 1 ? vsfm::WcsToD(params[1], NULL) : 1.0;
                const double nr = CalcAnimation(radius, style.mod_iglowRadius, fAnimate);
                const double ns = CalcAnimation(strength, style.mod_iglowStrength, fAnimate);
                style.mod_iglowRadius = !(nr > 0) ? 0 : nr > 4096 ? 4096 : nr;
                style.mod_iglowStrength = !(ns > 0) ? 0 : ns > 64 ? 64 : ns;
            }
            else
            {
                style.mod_iglowRadius = org.mod_iglowRadius;
                style.mod_iglowStrength = org.mod_iglowStrength;
            }
        }
        else if(cmd == L"mblur")
        {
            if(params.GetCount() > 0)
            {
                const double length = vsfm::WcsToD(params[0], NULL);
                const double angle = params.GetCount() > 1 ? vsfm::WcsToD(params[1], NULL) : 0.0;
                const double nl = CalcAnimation(length, style.mod_mblurLength, fAnimate);
                const double na = CalcAnimation(angle, style.mod_mblurAngle, fAnimate);
                style.mod_mblurLength = !(nl > 0) ? 0 : nl > 512 ? 512 : nl;
                style.mod_mblurAngle = na;
            }
            else
            {
                style.mod_mblurLength = org.mod_mblurLength;
                style.mod_mblurAngle = org.mod_mblurAngle;
            }
        }
        else if(cmd == L"bevel")
        {
            if(params.GetCount() > 0)
            {
                const double size = vsfm::WcsToD(params[0], NULL);
                const double strength = params.GetCount() > 1 ? vsfm::WcsToD(params[1], NULL) : 1.0;
                const double nz = CalcAnimation(size, style.mod_bevelSize, fAnimate);
                const double ns = CalcAnimation(strength, style.mod_bevelStrength, fAnimate);
                style.mod_bevelSize = !(nz > 0) ? 0 : nz > 256 ? 256 : nz;
                style.mod_bevelStrength = !(ns > 0) ? 0 : ns > 64 ? 64 : ns;
            }
            else
            {
                style.mod_bevelSize = org.mod_bevelSize;
                style.mod_bevelStrength = org.mod_bevelStrength;
            }
        }
        else if(cmd == L"clipf")
        {
            if(params.GetCount() > 0)
            {
                const double feather = vsfm::WcsToD(params[0], NULL);
                const double nf = CalcAnimation(feather, sub->m_clipFeather, fAnimate);
                sub->m_clipFeather = !(nf > 0) ? 0 : nf > 1024 ? 1024 : nf;
            }
        }
#endif
#ifdef _VSMOD // patch m011. jitter
        else if(cmd == L"jitter") // {\jitter(left,right,up,down,period,[seed])}
        {
            if((params.GetCount() >= 4))
            {
                int left = (int)abs(vsfm::WcsToL(params[0], NULL, 10)) * 8;
                int right = (int)abs(vsfm::WcsToL(params[1], NULL, 10)) * 8;
                int up = (int)abs(vsfm::WcsToL(params[2], NULL, 10)) * 8;
                int down = (int)abs(vsfm::WcsToL(params[3], NULL, 10)) * 8;
                style.mod_jitter.offset.top = CalcAnimation(up, style.mod_jitter.offset.top, fAnimate);
                style.mod_jitter.offset.bottom = CalcAnimation(down, style.mod_jitter.offset.bottom, fAnimate);
                style.mod_jitter.offset.left = CalcAnimation(left, style.mod_jitter.offset.left, fAnimate);
                style.mod_jitter.offset.right = CalcAnimation(right, style.mod_jitter.offset.right, fAnimate);
                style.mod_jitter.enabled = true;

                if(params.GetCount() >= 5)
                {
                    int period = vsfm::WcsToL(params[4], NULL, 10) * 10000;
                    style.mod_jitter.period = CalcAnimation(period, style.mod_jitter.period, fAnimate);
                    if(params.GetCount() >= 6)
                    {
                        style.mod_jitter.seed = vsfm::WcsToL(params[5], NULL, 10);
                    }
                }
            }
        }
#endif
        else if(cmd == L"kt")
        {
            m_kstart = !p.IsEmpty()
                       ? vsfm::WcsToL(p, NULL, 10) * 10
                       : 0;
            m_kend = m_kstart;
        }
        else if(cmd == L"kf" || cmd == L"K")
        {
            m_ktype = 1;
            m_kstart = m_kend;
            m_kend += !p.IsEmpty()
                      ? vsfm::WcsToL(p, NULL, 10) * 10
                      : 1000;
        }
        else if(cmd == L"ko")
        {
            m_ktype = 2;
            m_kstart = m_kend;
            m_kend += !p.IsEmpty()
                      ? vsfm::WcsToL(p, NULL, 10) * 10
                      : 1000;
        }
        else if(cmd == L"k")
        {
            m_ktype = 0;
            m_kstart = m_kend;
            m_kend += !p.IsEmpty()
                      ? vsfm::WcsToL(p, NULL, 10) * 10
                      : 1000;
        }
#ifdef _VSMOD // patch m005. add some move types
        else if(cmd == L"mover") // {\mover(x1,x2,x2,y2,alp1,alp2,r1,r2,t1,t2)}
        {
            if((params.GetCount() == 8 || params.GetCount() == 10) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = 1; // Radial move
                    e->param[1] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8); // x1
                    e->param[2] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8); // y1
                    e->param[3] = (int)(sub->m_scalex * vsfm::WcsToD(params[2], NULL) * 8); // x2
                    e->param[4] = (int)(sub->m_scaley * vsfm::WcsToD(params[3], NULL) * 8); // y2
                    e->param[5] = (int)(vsfm::WcsToD(params[4], NULL) * 10000); // alp1
                    e->param[6] = (int)(vsfm::WcsToD(params[5], NULL) * 10000); // alp2
                    e->param[7] = (int)(sub->m_scalex * vsfm::WcsToD(params[6], NULL) * 8); // r1
                    e->param[8] = (int)(sub->m_scaley * vsfm::WcsToD(params[7], NULL) * 8); // r2

                    e->t[0] = e->t[1] = -1;

                    if(params.GetCount() == 10)
                    {
                        for(int i = 0; i < 2; i++)
                            e->t[i] = vsfm::WcsToL(params[8+i], NULL, 10);
                    }
                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
        else if(cmd == L"moves3") // {\moves3(x1,x2,x2,y2,x3,y3[,t1,t2])}
        {
            if((params.GetCount() == 6 || params.GetCount() == 8) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = 2; // square spline
                    e->param[1] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8); // x1
                    e->param[2] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8); // y1
                    e->param[3] = (int)(sub->m_scalex * vsfm::WcsToD(params[2], NULL) * 8); // x2
                    e->param[4] = (int)(sub->m_scaley * vsfm::WcsToD(params[3], NULL) * 8); // y2
                    e->param[5] = (int)(sub->m_scalex * vsfm::WcsToD(params[4], NULL) * 8); // x3
                    e->param[6] = (int)(sub->m_scaley * vsfm::WcsToD(params[5], NULL) * 8); // y3
                    e->t[0] = e->t[1] = -1;

                    if(params.GetCount() == 8)
                    {
                        for(int i = 0; i < 2; i++)
                            e->t[i] = vsfm::WcsToL(params[6+i], NULL, 10);
                    }
                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
        else if(cmd == L"moves4") // {\moves4(x1,x2,x2,y2,x3,y3,x4,y4[,t1,t2])}
        {
            if((params.GetCount() == 8 || params.GetCount() == 10) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = 3; // cubic spline
                    e->param[1] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8); // x1
                    e->param[2] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8); // y1
                    e->param[3] = (int)(sub->m_scalex * vsfm::WcsToD(params[2], NULL) * 8); // x2
                    e->param[4] = (int)(sub->m_scaley * vsfm::WcsToD(params[3], NULL) * 8); // y2
                    e->param[5] = (int)(sub->m_scalex * vsfm::WcsToD(params[4], NULL) * 8); // x3
                    e->param[6] = (int)(sub->m_scaley * vsfm::WcsToD(params[5], NULL) * 8); // y3
                    e->param[7] = (int)(sub->m_scalex * vsfm::WcsToD(params[6], NULL) * 8); // x4
                    e->param[8] = (int)(sub->m_scaley * vsfm::WcsToD(params[7], NULL) * 8); // y4
                    e->t[0] = e->t[1] = -1;

                    if(params.GetCount() == 10)
                    {
                        for(int i = 0; i < 2; i++)
                            e->t[i] = vsfm::WcsToL(params[8+i], NULL, 10);
                    }
                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
#endif
#ifdef _VSMOD // patch m006. moveable vector clip
        else if(cmd == L"movevc")
        {
            if((params.GetCount() == 2 || params.GetCount() == 4 || params.GetCount() == 6) && !sub->m_effects[EF_VECTCLP])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL));
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL));
                    e->t[0] = e->t[1] = -1;

                    if(params.GetCount() >= 4)
                    {
                        e->param[2] = (int)(sub->m_scalex * vsfm::WcsToD(params[2], NULL));
                        e->param[3] = (int)(sub->m_scaley * vsfm::WcsToD(params[3], NULL));
                    }
                    if(params.GetCount() == 6)
                    {
                        e->t[0] = (int)(sub->m_scalex * vsfm::WcsToD(params[4], NULL));
                        e->t[1] = (int)(sub->m_scaley * vsfm::WcsToD(params[5], NULL));
                    }
                    sub->m_effects[EF_VECTCLP] = e;
                }
            }
        }
#endif
        else if(cmd == L"move") // {\move(x1=param[0], y1=param[1], x2=param[2], y2=param[3][, t1=t[0], t2=t[1]])}
        {
            if((params.GetCount() == 4 || params.GetCount() == 6) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = DNew Effect)
                {
#ifdef _VSMOD // patch m005. add some move types
                    e->param[0] = 0; // обычный мов
                    e->param[1] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8);
                    e->param[2] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8);
                    e->param[3] = (int)(sub->m_scalex * vsfm::WcsToD(params[2], NULL) * 8);
                    e->param[4] = (int)(sub->m_scaley * vsfm::WcsToD(params[3], NULL) * 8);
#else
                    e->param[0] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8);
                    e->param[1] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8);
                    e->param[2] = (int)(sub->m_scalex * vsfm::WcsToD(params[2], NULL) * 8);
                    e->param[3] = (int)(sub->m_scaley * vsfm::WcsToD(params[3], NULL) * 8);
#endif
                    e->t[0] = e->t[1] = -1;

                    if(params.GetCount() == 6)
                    {
                        for(ptrdiff_t i = 0; i < 2; i++)
                            e->t[i] = vsfm::WcsToL(params[4+i], NULL, 10);
                    }

                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
        else if(cmd == L"org") // {\org(x=param[0], y=param[1])}
        {
#ifdef _VSMOD // patch f003. moving \org for some karaoke effects. part 1
            if((params.GetCount() == 2 || params.GetCount() == 4 || params.GetCount() == 6) && !sub->m_effects[EF_ORG])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8);
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8);
                    e->t[0] = e->t[1] = -1;

                    if(params.GetCount() >= 4)
                    {
                        e->param[2] = (int)(sub->m_scalex * vsfm::WcsToD(params[2], NULL) * 8);
                        e->param[3] = (int)(sub->m_scaley * vsfm::WcsToD(params[3], NULL) * 8);
                    }
                    if(params.GetCount() == 6)
                    {
                        e->t[0] = (int)(sub->m_scalex * vsfm::WcsToD(params[4], NULL) * 8);
                        e->t[1] = (int)(sub->m_scaley * vsfm::WcsToD(params[5], NULL) * 8);
                    }
                    sub->m_effects[EF_ORG] = e;
                }
            }
#else
            if(params.GetCount() == 2 && !sub->m_effects[EF_ORG])
            {
                if(Effect* e = DNew Effect)
                {
                    e->param[0] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8);
                    e->param[1] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8);

                    sub->m_effects[EF_ORG] = e;
                }
            }
#endif
        }
        else if(cmd == L"pbo")
        {
            m_polygonBaselineOffset = vsfm::WcsToL(p, NULL, 10);
        }
        else if(cmd == L"pos")
        {
            if(params.GetCount() == 2 && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = DNew Effect)
                {
#ifdef _VSMOD // patch m005. add some move types
                    e->param[0] = 0; // usual move
                    e->param[1] = e->param[3] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8);
                    e->param[2] = e->param[4] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8);
#else
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8);
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8);
#endif
                    e->t[0] = e->t[1] = 0;

                    sub->m_effects[EF_MOVE] = e;
                }
            }
#ifdef _VSMOD // patch m002. Z-coord
            else if(params.GetCount() == 3 && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = DNew Effect)
                {
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * vsfm::WcsToD(params[0], NULL) * 8);
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * vsfm::WcsToD(params[1], NULL) * 8);
                    e->t[0] = e->t[1] = 0;

                    sub->m_effects[EF_MOVE] = e;
                    style.mod_z = vsfm::WcsToD(params[2], NULL) * 80;
                }
            }
#endif
        }
        else if(cmd == L"p")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            m_nPolygon = (n <= 0 ? 0 : n);
        }
        else if(cmd == L"q")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            sub->m_wrapStyle = !p.IsEmpty() && (0 <= n && n <= 3)
                               ? n
                               : m_defaultWrapStyle;
        }
#ifdef _VSMOD // patch m003. random text points
        else if(cmd == L"rnds")
        {
            double dst = vsfm::WcsToL(p, NULL, 16);
            double nx = CalcAnimation(dst, style.mod_rand.Seed, fAnimate);
            style.mod_rand.Seed = !p.IsEmpty() ? nx : org.mod_rand.Seed;
        }
        else if(cmd == L"rndx")
        {
            double dst = vsfm::WcsToD(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_rand.X, fAnimate);
            style.mod_rand.X = !p.IsEmpty() ? nx : org.mod_rand.X;
        }
        else if(cmd == L"rndy")
        {
            double dst = vsfm::WcsToD(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_rand.Y, fAnimate);
            style.mod_rand.Y = !p.IsEmpty() ? nx : org.mod_rand.Y;
        }
        else if(cmd == L"rndz")
        {
            double dst = vsfm::WcsToD(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_rand.Z, fAnimate);
            style.mod_rand.Z = !p.IsEmpty() ? nx : org.mod_rand.Z;
        }
        else if(cmd == L"rnd")
        {
            double dst = vsfm::WcsToD(p, NULL) * 8;
            style.mod_rand.X = !p.IsEmpty() ? CalcAnimation(dst, style.mod_rand.X, fAnimate) : org.mod_rand.X;
            style.mod_rand.Y = !p.IsEmpty() ? CalcAnimation(dst, style.mod_rand.Y, fAnimate) : org.mod_rand.Y;
            style.mod_rand.Z = !p.IsEmpty() ? CalcAnimation(dst, style.mod_rand.Z, fAnimate) : org.mod_rand.Z;
        }
#endif
        else if(cmd == L"r")
        {
            STSStyle* val;
            style = (!p.IsEmpty() && m_styles.Lookup(CString(p), val) && val) ? *val : org;
        }
        else if(cmd == L"shad")
        {
            double dst = vsfm::WcsToD(p, NULL);
            double nx = CalcAnimation(dst, style.shadowDepthX, fAnimate);
            style.shadowDepthX = !p.IsEmpty()
                                 ? (nx < 0 ? 0 : nx)
                                     : org.shadowDepthX;
            double ny = CalcAnimation(dst, style.shadowDepthY, fAnimate);
            style.shadowDepthY = !p.IsEmpty()
                                 ? (ny < 0 ? 0 : ny)
                                     : org.shadowDepthY;
        }
        else if(cmd == L"s")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            style.fStrikeOut = !p.IsEmpty()
                               ? (n == 0 ? false : n == 1 ? true : org.fStrikeOut)
                                   : org.fStrikeOut;
        }
        else if(cmd == L"t") // \t([<t1>,<t2>,][<accel>,]<style modifiers>)
        {
            p.Empty();

            m_animStart = m_animEnd = 0;
            m_animAccel = 1;

            if(params.GetCount() == 1)
            {
                p = params[0];
            }
            else if(params.GetCount() == 2)
            {
                m_animAccel = vsfm::WcsToD(params[0], NULL);
                p = params[1];
            }
            else if(params.GetCount() == 3)
            {
                m_animStart = (int)vsfm::WcsToD(params[0], NULL);
                m_animEnd = (int)vsfm::WcsToD(params[1], NULL);
                p = params[2];
            }
            else if(params.GetCount() == 4)
            {
                m_animStart = vsfm::WcsToL(params[0], NULL, 10);
                m_animEnd = vsfm::WcsToL(params[1], NULL, 10);
                m_animAccel = vsfm::WcsToD(params[2], NULL);
                p = params[3];
            }

            ParseSSATag(sub, p, style, org, true);

            sub->m_fAnimated = true;
        }
        else if(cmd == L"u")
        {
            int n = vsfm::WcsToL(p, NULL, 10);
            style.fUnderline = !p.IsEmpty()
                               ? (n == 0 ? false : n == 1 ? true : org.fUnderline)
                                   : org.fUnderline;
        }
        else if(cmd == L"xbord")
        {
            double dst = vsfm::WcsToD(p, NULL);
            double nx = CalcAnimation(dst, style.outlineWidthX, fAnimate);
            style.outlineWidthX = !p.IsEmpty()
                                  ? (nx < 0 ? 0 : nx)
                                      : org.outlineWidthX;
        }
        else if(cmd == L"xshad")
        {
            double dst = vsfm::WcsToD(p, NULL);
            double nx = CalcAnimation(dst, style.shadowDepthX, fAnimate);
            style.shadowDepthX = !p.IsEmpty()
                                 ? nx
                                 : org.shadowDepthX;
        }
        else if(cmd == L"ybord")
        {
            double dst = vsfm::WcsToD(p, NULL);
            double ny = CalcAnimation(dst, style.outlineWidthY, fAnimate);
            style.outlineWidthY = !p.IsEmpty()
                                  ? (ny < 0 ? 0 : ny)
                                      : org.outlineWidthY;
        }
        else if(cmd == L"yshad")
        {
            double dst = vsfm::WcsToD(p, NULL);
            double ny = CalcAnimation(dst, style.shadowDepthY, fAnimate);
            style.shadowDepthY = !p.IsEmpty()
                                 ? ny
                                 : org.shadowDepthY;
        }
#ifdef _VSMOD // patch m002. Z-coord
        else if(cmd == L"z")
        {
            double dst = vsfm::WcsToD(p, NULL) * 80;
            double nx = CalcAnimation(dst, style.mod_z, fAnimate);
            style.mod_z = !p.IsEmpty() ? nx : org.mod_z;
        }
#endif
    }

//	return(nUnrecognizedTags < nTags);
    return(true); // there are ppl keeping coments inside {}, lets make them happy now
}

bool CRenderedTextSubtitle::ParseHtmlTag(CSubtitle* sub, CStringW str, STSStyle& style, STSStyle& org)
{
    if(str.Find(L"!--") == 0)
        return(true);

    bool fClosing = str[0] == '/';
    str.Trim(L" /");

    int i = str.Find(' ');
    if(i < 0) i = str.GetLength();

    CStringW tag = str.Left(i).MakeLower();
    str = str.Mid(i).Trim();

    CAtlArray<CStringW> attribs, params;
    while((i = str.Find('=')) > 0)
    {
        attribs.Add(str.Left(i).Trim().MakeLower());
        str = str.Mid(i + 1);
        for(i = 0; _istspace(str[i]); i++);
        str = str.Mid(i);
        if(str[0] == '\"')
        {
            str = str.Mid(1);
            i = str.Find('\"');
        }
        else i = str.Find(' ');
        if(i < 0) i = str.GetLength();
        params.Add(str.Left(i).Trim().MakeLower());
        str = str.Mid(i + 1);
    }

    if(tag == L"text")
        ;
    else if(tag == L"b" || tag == L"strong")
        style.fontWeight = !fClosing ? FW_BOLD : org.fontWeight;
    else if(tag == L"i" || tag == L"em")
        style.fItalic = !fClosing ? true : org.fItalic;
    else if(tag == L"u")
        style.fUnderline = !fClosing ? true : org.fUnderline;
    else if(tag == L"s" || tag == L"strike" || tag == L"del")
        style.fStrikeOut = !fClosing ? true : org.fStrikeOut;
    else if(tag == L"font")
    {
        if(!fClosing)
        {
            for(i = 0; i < attribs.GetCount(); i++)
            {
                if(params[i].IsEmpty()) continue;

                int nColor = -1;

                if(attribs[i] == L"face")
                {
                    style.fontName = params[i];
                }
                else if(attribs[i] == L"size")
                {
                    if(params[i][0] == '+')
                        style.fontSize += vsfm::WcsToL(params[i], NULL, 10);
                    else if(params[i][0] == '-')
                        style.fontSize -= vsfm::WcsToL(params[i], NULL, 10);
                    else
                        style.fontSize = vsfm::WcsToL(params[i], NULL, 10);
                }
                else if(attribs[i] == L"color")
                {
                    nColor = 0;
                }
                else if(attribs[i] == L"outline-color")
                {
                    nColor = 2;
                }
                else if(attribs[i] == L"outline-level")
                {
                    style.outlineWidthX = style.outlineWidthY = vsfm::WcsToL(params[i], NULL, 10);
                }
                else if(attribs[i] == L"shadow-color")
                {
                    nColor = 3;
                }
                else if(attribs[i] == L"shadow-level")
                {
                    style.shadowDepthX = style.shadowDepthY = vsfm::WcsToL(params[i], NULL, 10);
                }

                if(nColor >= 0 && nColor < 4)
                {
                    CString key = WToT(params[i]).TrimLeft('#');
                    DWORD val;
                    if(g_colors.Lookup(key, val))
                        style.colors[nColor] = val;
                    else if((style.colors[nColor] = _tcstol(key, NULL, 16)) == 0)
                        style.colors[nColor] = 0x00ffffff;  // default is white
                    style.colors[nColor] = ((style.colors[nColor] >> 16) & 0xff) | ((style.colors[nColor] & 0xff) << 16) | (style.colors[nColor] & 0x00ff00);
                }
            }
        }
        else
        {
            style.fontName = org.fontName;
            style.fontSize = org.fontSize;
            memcpy(style.colors, org.colors, sizeof(style.colors));
        }
    }
    else if(tag == L"k" && attribs.GetCount() == 1 && attribs[0] == L"t")
    {
        m_ktype = 1;
        m_kstart = m_kend;
        m_kend += vsfm::WcsToL(params[0], NULL, 10);
    }
    else
        return(false);

    return(true);
}

double CRenderedTextSubtitle::CalcAnimation(double dst, double src, bool fAnimate)
{
    int s = m_animStart ? m_animStart : 0;
    int e = m_animEnd ? m_animEnd : m_delay;

    if(fabs(dst - src) >= 0.0001 && fAnimate)
    {
        if(m_time < s) dst = src;
        else if(s <= m_time && m_time < e)
        {
            double t = vsfm::DetPow(1.0 * (m_time - s) / (e - s), m_animAccel);
            dst = (1 - t) * src + t * dst;
        }
//		else dst = dst;
    }

    return(dst);
}

CSubtitle* CRenderedTextSubtitle::GetSubtitle(int entry)
{
    CSubtitle* sub;
    if(m_subtitleCache.Lookup(entry, sub))
    {
        if(sub->m_fAnimated)
        {
            delete sub;
            sub = NULL;
        }
        else return(sub);
    }

    sub = DNew CSubtitle();
    if(!sub) return(NULL);

    CStringW str = GetStrW(entry, true);

    STSStyle stss, orgstss;
    if(m_doOverrideStyle && m_pStyleOverride != NULL)
    {
        // this RTS has been signaled to ignore embedded styles, use the built-in one
        stss = *m_pStyleOverride;
    }
    else
    {
        // find the appropriate embedded style
        GetStyle(entry, stss);
    }
    if(m_ePARCompensationType == EPCTUpscale)
    {
        if(stss.fontScaleX / stss.fontScaleY == 1.0 && m_dPARCompensation != 1.0)
        {
            if(m_dPARCompensation < 1.0)
                stss.fontScaleY /= m_dPARCompensation;
            else
                stss.fontScaleX *= m_dPARCompensation;
        }
    }
    else if(m_ePARCompensationType == EPCTDownscale)
    {
        if(stss.fontScaleX / stss.fontScaleY == 1.0 && m_dPARCompensation != 1.0)
        {
            if(m_dPARCompensation < 1.0)
                stss.fontScaleX *= m_dPARCompensation;
            else
                stss.fontScaleY /= m_dPARCompensation;
        }
    }
    else if(m_ePARCompensationType == EPCTAccurateSize)
    {
        if(stss.fontScaleX / stss.fontScaleY == 1.0 && m_dPARCompensation != 1.0)
        {
            stss.fontScaleX *= m_dPARCompensation;
        }
    }

    orgstss = stss;

    sub->m_clip.SetRect(0, 0, m_size.cx >> 3, m_size.cy >> 3);
    sub->m_scrAlignment = -stss.scrAlignment;
    sub->m_wrapStyle = m_defaultWrapStyle;
    sub->m_fAnimated = false;
    sub->m_relativeTo = stss.relativeTo;
    // this whole conditional is a work-around for what happens in STS.cpp:
    // in CSimpleTextSubtitle::Open, we have m_dstScreenSize = CSize(384, 288)
    // now, files containing embedded subtitles (and with styles) set m_dstScreenSize to a correct value
    // but where no style is given, those defaults are taken - 384, 288
    if(m_doOverrideStyle && m_pStyleOverride != NULL)
    {
        // so mind the default values, stated here to increase comprehension
        sub->m_scalex = (stss.relativeTo == 1 ? m_vidrect.Width() : m_size.cx) / (384 * 8);
        sub->m_scaley = (stss.relativeTo == 1 ? m_vidrect.Height() : m_size.cy) / (288 * 8);
    }
    else
    {
        sub->m_scalex = m_dstScreenSize.cx > 0 ? 1.0 * (stss.relativeTo == 1 ? m_vidrect.Width() : m_size.cx) / (m_dstScreenSize.cx * 8) : 1.0;
        sub->m_scaley = m_dstScreenSize.cy > 0 ? 1.0 * (stss.relativeTo == 1 ? m_vidrect.Height() : m_size.cy) / (m_dstScreenSize.cy * 8) : 1.0;
    }

    m_animStart = m_animEnd = 0;
    m_animAccel = 1;
    m_ktype = m_kstart = m_kend = 0;
    m_nPolygon = 0;
    m_polygonBaselineOffset = 0;
#ifdef _VSMOD // patch m004. gradient colors
    // allow init gradient without \$vc \$va
    for (int i = 0; i < 4; i++)
    {
        COLORREF revClr = revcolor(stss.colors[i]);

        for (int j = 0; j < 4; j++)
        {
            stss.mod_grad.alpha[i][j] = stss.alpha[i];
            stss.mod_grad.color[i][j] = revClr;
        }
    }
#endif
    ParseEffect(sub, GetAt(entry).effect);

    while(!str.IsEmpty())
    {
        bool fParsed = false;

        int i;

        if(str[0] == '{' && (i = str.Find(L'}')) > 0)
        {
            if(fParsed = ParseSSATag(sub, str.Mid(1, i - 1), stss, orgstss))
                str = str.Mid(i + 1);
        }
        else if(str[0] == '<' && (i = str.Find(L'>')) > 0)
        {
            if(fParsed = ParseHtmlTag(sub, str.Mid(1, i - 1), stss, orgstss))
                str = str.Mid(i + 1);
        }

        if(fParsed)
        {
            i = str.FindOneOf(L"{<");
            if(i < 0) i = str.GetLength();
            if(i == 0) continue;
        }
        else
        {
            i = str.Mid(1).FindOneOf(L"{<");
            if(i < 0) i = str.GetLength() - 1;
            i++;
        }

        STSStyle tmp;

        tmp = stss;

        tmp.fontSize = sub->m_scaley * tmp.fontSize * 64;
        tmp.fontSpacing = sub->m_scalex * tmp.fontSpacing * 64;
        tmp.outlineWidthX *= (m_fScaledBAS ? sub->m_scalex : 1) * 8;
        tmp.outlineWidthY *= (m_fScaledBAS ? sub->m_scaley : 1) * 8;
        tmp.shadowDepthX *= (m_fScaledBAS ? sub->m_scalex : 1) * 8;
        tmp.shadowDepthY *= (m_fScaledBAS ? sub->m_scaley : 1) * 8;
#ifdef _VSMOD // patch m012. glow
        tmp.mod_glowRadius *= 8;
        tmp.mod_iglowRadius *= 8;
#endif

        if(m_nPolygon)
        {
            ParsePolygon(sub, str.Left(i), tmp);
        }
        else
        {
            ParseString(sub, str.Left(i), tmp);
        }

        str = str.Mid(i);
    }

    // just a "work-around" solution... in most cases nobody will want to use \org together with moving but without rotating the subs
    if(sub->m_effects[EF_ORG] && (sub->m_effects[EF_MOVE] || sub->m_effects[EF_BANNER] || sub->m_effects[EF_SCROLL]))
        sub->m_fAnimated = true;

    sub->m_scrAlignment = abs(sub->m_scrAlignment);

    STSEntry stse = GetAt(entry);
    CRect marginRect = stse.marginRect;
    if(marginRect.left == 0) marginRect.left = orgstss.marginRect.left;
    if(marginRect.top == 0) marginRect.top = orgstss.marginRect.top;
    if(marginRect.right == 0) marginRect.right = orgstss.marginRect.right;
    if(marginRect.bottom == 0) marginRect.bottom = orgstss.marginRect.bottom;
    marginRect.left = (int)(sub->m_scalex * marginRect.left * 8);
    marginRect.top = (int)(sub->m_scaley * marginRect.top * 8);
    marginRect.right = (int)(sub->m_scalex * marginRect.right * 8);
    marginRect.bottom = (int)(sub->m_scaley * marginRect.bottom * 8);

    if(stss.relativeTo == 1)
    {
        marginRect.left += m_vidrect.left;
        marginRect.top += m_vidrect.top;
        marginRect.right += m_size.cx - m_vidrect.right;
        marginRect.bottom += m_size.cy - m_vidrect.bottom;
    }

    sub->CreateClippers(m_size);

    sub->MakeLines(m_size, marginRect);

    m_subtitleCache[entry] = sub;

    return(sub);
}

//

// Frame lookup and rendering (formerly ISubPicProvider)

POSITION CRenderedTextSubtitle::GetStartPosition(REFERENCE_TIME rt, double fps)
{
    int iSegment = -1;
    SearchSubs((int)(rt / 10000), fps, &iSegment, NULL);

    if(iSegment < 0) iSegment = 0;

    return(GetNext((POSITION)iSegment));
}

POSITION CRenderedTextSubtitle::GetNext(POSITION pos)
{
    int iSegment = (int)(intptr_t)pos;

    const STSSegment* stss;
    while((stss = GetSegment(iSegment)) && stss->subs.GetCount() == 0)
        iSegment++;

    return(stss ? (POSITION)(intptr_t)(iSegment + 1) : NULL);
}

REFERENCE_TIME CRenderedTextSubtitle::GetStart(POSITION pos, double fps)
{
    return(10000LL * TranslateSegmentStart((int)(intptr_t)pos - 1, fps));
}

REFERENCE_TIME CRenderedTextSubtitle::GetStop(POSITION pos, double fps)
{
    return(10000LL * TranslateSegmentEnd((int)(intptr_t)pos - 1, fps));
}

bool CRenderedTextSubtitle::IsAnimated(POSITION pos)
{
    return(true);
}

struct LSub
{
    int idx, layer, readorder;
};

/* Layer, then read order; stable, so equal keys keep the segment's order on
 * every platform (see CSimpleTextSubtitle::Sort). */
static bool LSubLess(const LSub& a, const LSub& b)
{
    if(a.layer != b.layer) return a.layer < b.layer;
    return a.readorder < b.readorder;
}

HRESULT CRenderedTextSubtitle::Render(SubPicDesc& spd, REFERENCE_TIME rt, double fps, RECT& bbox)
{
    CRect bbox2(0, 0, 0, 0);

#ifdef _VSMOD // patch m016. track matte
    /* Per-frame matte state, keyed by the matte events' layer. Matte events
     * render into a shared per-layer canvas; after each matte event the
     * coverage is re-extracted so events on the target layer (matte layer +
     * offset, always above, hence already processed in this bottom-to-top
     * pass) can consume it. */
    struct MatteLayerState
    {
        bool inverse = false;
        int targetOffset = 1;
        bool hasCoverage = false;
        std::vector<unsigned int> pixels;
        std::vector<BYTE> coverage;
    };
    std::map<int, MatteLayerState> matteLayers;
#endif

    if(m_size != CSize(spd.w * 8, spd.h * 8) || m_vidrect != CRect(spd.vidrect.left * 8, spd.vidrect.top * 8, spd.vidrect.right * 8, spd.vidrect.bottom * 8))
        Init(CSize(spd.w, spd.h), spd.vidrect);

    int t = (int)(rt / 10000);

    int segment;
//    TRACE(_T("search started: %d"), t);
    const STSSegment* stss = SearchSubs(t, fps, &segment);
//    TRACE(_T("search complete: %d"), t);
    if(!stss) return S_FALSE;

    // clear any cached subs that current position is not in its bounds
    {
        POSITION pos = m_subtitleCache.GetStartPosition();
        while(pos)
        {
            int key;
            CSubtitle* value;
            m_subtitleCache.GetNextAssoc(pos, key, value);

            STSEntry& stse = GetAt(key);
            if (stse.end <= (t) || stse.start > (t))
            {
                delete value;
                m_subtitleCache.RemoveKey(key);
                pos = m_subtitleCache.GetStartPosition();
            }
        }
    }

    m_sla.AdvanceToSegment(segment, stss->subs);

    CAtlArray<LSub> subs;

    for(ptrdiff_t i = 0, j = stss->subs.GetCount(); i < j; i++)
    {
        LSub ls;
        ls.idx = stss->subs[i];
        ls.layer = GetAt(stss->subs[i]).layer;
        ls.readorder = GetAt(stss->subs[i]).readorder;
        subs.Add(ls);
    }

    std::stable_sort(subs.GetData(), subs.GetData() + subs.GetCount(), LSubLess);

    for(ptrdiff_t i = 0, j = subs.GetCount(); i < j; i++)
    {
        int entry = subs[i].idx;

        STSEntry stse = GetAt(entry);

        {
            int start = TranslateStart(entry, fps);
            m_time = t - start;
            m_delay = TranslateEnd(entry, fps) - start;
        }

        CSubtitle* s = GetSubtitle(entry);
        if(!s) continue;

        /* patch m016. track matte: redirect all painting of matte/matted
         * events into offscreen canvases. tspd starts as a copy of the frame
         * descriptor (identical dimensions); for redirected events only the
         * pixel pointer and pitch change, so every dimension read below is
         * unaffected and the normal path is bit-identical. tbbox likewise
         * aliases the shared bbox2 unless redirected. */
        SubPicDesc tspd = spd;
        CRect localBBox(0, 0, 0, 0);
        CRect* ptbbox = &bbox2;
#ifdef _VSMOD
        const bool isMatteEvent = (s->m_matteMode != 0);
        const MatteLayerState* consumeMatte = NULL;
        MatteLayerState* myMatteState = NULL;
        std::vector<unsigned int> evtPixels;
        if(isMatteEvent)
        {
            MatteLayerState& ms = matteLayers[subs[i].layer];
            if(ms.pixels.empty())
            {
                ms.pixels.assign((size_t)spd.w * spd.h, 0xff000000);
                ms.inverse = (s->m_matteMode == 2);
                ms.targetOffset = s->m_matteOffset;
            }
            myMatteState = &ms;
            tspd.bits = (BYTE*)ms.pixels.data();
            tspd.pitch = spd.w * 4;
            ptbbox = &localBBox;
        }
        else
        {
            for(std::map<int, MatteLayerState>::const_reverse_iterator it = matteLayers.rbegin(); it != matteLayers.rend(); ++it)
            {
                if(it->second.hasCoverage && it->first + it->second.targetOffset == subs[i].layer)
                {
                    consumeMatte = &it->second;
                    break; /* highest matte layer below wins */
                }
            }
            if(consumeMatte)
            {
                evtPixels.assign((size_t)spd.w * spd.h, 0xff000000);
                tspd.bits = (BYTE*)evtPixels.data();
                tspd.pitch = spd.w * 4;
                ptbbox = &localBBox;
            }
        }
#endif
        CRect& tbbox = *ptbbox;

        CRect clipRect = s->m_clip;
        CRect r = s->m_rect;
        CSize spaceNeeded = r.Size();

        // apply the effects

        bool fPosOverride = false, fOrgOverride = false;

        int alpha = 0x00;

        CPoint org2;

        BYTE* pAlphaMask = s->m_pClipper ? s->m_pClipper->m_pAlphaMask : NULL;

#ifdef _VSMOD // patch m006. moveable vector clip
        MOD_MOVEVC mod_vc;
        mod_vc.spd = CSize(tspd.w, tspd.h);
        //mod_vc.alphamask = pAlphaMask;
        mod_vc.size = s->m_pClipper ? s->m_pClipper->m_size : CSize(0, 0);
#endif

        for(int k = 0; k < EF_NUMBEROFEFFECTS; k++)
        {
            if(!s->m_effects[k]) continue;

            switch(k)
            {
            case EF_MOVE: // {\move(x1=param[0], y1=param[1], x2=param[2], y2=param[3], t1=t[0], t2=t[1])}
            {
                CPoint p;
#ifdef _VSMOD // patch m005. add some move types
                if(s->m_effects[k]->param[0] == 0)
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]);
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]);
#else
                CPoint p1(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
                CPoint p2(s->m_effects[k]->param[2], s->m_effects[k]->param[3]);
#endif
                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = p1;
                    else if(p1 == p2) p = p1;
                    else if(t1 < m_time && m_time < t2)
                    {
                        double t = 1.0 * (m_time - t1) / (t2 - t1);
                        p.x = (int)((1 - t) * p1.x + t * p2.x);
                        p.y = (int)((1 - t) * p1.y + t * p2.y);
                    }
                    else p = p2;
#ifdef _VSMOD // patch m005. add some move types
                }
                else if(s->m_effects[k]->param[0] == 1) // radial move
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]); // p0
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]); // p1
                    CPoint r0(s->m_effects[k]->param[7], s->m_effects[k]->param[8]);
                    double alp1 = s->m_effects[k]->param[5] * PI / 1800000; // alp1
                    double alp2 = s->m_effects[k]->param[6] * PI / 1800000; // alp2
                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    CPoint pr1(p1.x + vsfm::DetCos(alp1) * r0.x, p1.y - vsfm::DetSin(alp1) * r0.x);
                    CPoint pr2(p2.x + vsfm::DetCos(alp2) * r0.y, p2.y - vsfm::DetSin(alp2) * r0.y);

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = pr1;
                    //else if (p1 == p2) p = pr1; // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                    else if((t1 < m_time) && (m_time < t2))
                    {
                        double t = (double)(m_time - t1) / (t2 - t1);
                        double alp = ((1 - t) * alp1 + t * alp2);
                        double rt = ((1 - t) * r0.x + t * r0.y);

                        p.x = (int)((1 - t) * p1.x + t * p2.x); // origin point
                        p.y = (int)((1 - t) * p1.y + t * p2.y);
                        p.x += (int)(vsfm::DetCos(alp) * rt);
                        p.y -= (int)(vsfm::DetSin(alp) * rt);
                    }
                    else p = pr2;
                }
                else if(s->m_effects[k]->param[0] == 2) // square spline
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]);
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]);
                    CPoint p3(s->m_effects[k]->param[5], s->m_effects[k]->param[6]);

                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = p1;
                    else if(p1 == p2) p = p1;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                    else if(t1 < m_time && m_time < t2)
                    {
                        double t = (double)(m_time - t1) / (t2 - t1);
                        p.x = (int)((1 - t) * (1 - t) * p1.x + 2 * t * (1 - t) * p2.x + t * t * p3.x);
                        p.y = (int)((1 - t) * (1 - t) * p1.y + 2 * t * (1 - t) * p2.y + t * t * p3.y);
                    }
                    else p = p3;
                }
                else if(s->m_effects[k]->param[0] == 3) // cubic spline
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]);
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]);
                    CPoint p3(s->m_effects[k]->param[5], s->m_effects[k]->param[6]);
                    CPoint p4(s->m_effects[k]->param[7], s->m_effects[k]->param[8]);

                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = p1;
                    else if(p1 == p2) p = p1;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                    else if(t1 < m_time && m_time < t2)
                    {
                        double t = (double)(m_time - t1) / (t2 - t1);
                        p.x = (int)((1 - t) * (1 - t) * (1 - t) * p1.x + 3 * t * (1 - t) * (1 - t) * p2.x + 3 * t * t * (1 - t) * p3.x + t * t * t * p4.x);
                        p.y = (int)((1 - t) * (1 - t) * (1 - t) * p1.y + 3 * t * (1 - t) * (1 - t) * p2.y + 3 * t * t * (1 - t) * p3.y + t * t * t * p4.y);
                    }
                    else p = p4;
                }
#endif
                r = CRect(
                        CPoint((s->m_scrAlignment % 3) == 1 ? p.x : (s->m_scrAlignment % 3) == 0 ? p.x - spaceNeeded.cx : p.x - (spaceNeeded.cx + 1) / 2,
                               s->m_scrAlignment <= 3 ? p.y - spaceNeeded.cy : s->m_scrAlignment <= 6 ? p.y - (spaceNeeded.cy + 1) / 2 : p.y),
                        spaceNeeded);

                if(s->m_relativeTo == 1)
                    r.OffsetRect(m_vidrect.TopLeft());
                fPosOverride = true;
            }
            break;
            case EF_ORG: // {\org(x=param[0], y=param[1])}
            {
#ifdef _VSMOD // patch f003. moving \org for some karaoke effects
                CPoint orgA(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
                CPoint orgB(s->m_effects[k]->param[2], s->m_effects[k]->param[3]);
                int to1 = s->m_effects[k]->t[0];
                int to2 = s->m_effects[k]->t[1];

                if(to2 < to1)
                {
                    int to = to1;
                    to1 = to2;
                    to2 = to;
                }

                if(to1 <= 0 && to2 <= 0)
                {
                    to1 = 0;
                    to2 = m_delay;
                }

                if(m_time <= to1) org2 = orgA;
                else if(to1 == to2) org2 = orgA;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                else if(to1 < m_time && m_time < to2)
                {
                    double t = 1.0 * (m_time - to1) / (to2 - to1);
                    org2.x = (int)((1 - t) * orgA.x + t * orgB.x);
                    org2.y = (int)((1 - t) * orgA.y + t * orgB.y);
                }
                else org2 = orgB;
#else
                org2 = CPoint(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
#endif
                fOrgOverride = true;
            }
            break;
            case EF_FADE: // {\fade(a1=param[0], a2=param[1], a3=param[2], t1=t[0], t2=t[1], t3=t[2], t4=t[3]) or {\fad(t1=t[1], t2=t[2])
            {
                int t1 = s->m_effects[k]->t[0];
                int t2 = s->m_effects[k]->t[1];
                int t3 = s->m_effects[k]->t[2];
                int t4 = s->m_effects[k]->t[3];

                if(t1 == -1 && t4 == -1)
                {
                    t1 = 0;
                    t3 = m_delay - t3;
                    t4 = m_delay;
                }

                if(m_time < t1) alpha = s->m_effects[k]->param[0];
                else if(m_time >= t1 && m_time < t2)
                {
                    double t = 1.0 * (m_time - t1) / (t2 - t1);
                    alpha = (int)(s->m_effects[k]->param[0] * (1 - t) + s->m_effects[k]->param[1] * t);
                }
                else if(m_time >= t2 && m_time < t3) alpha = s->m_effects[k]->param[1];
                else if(m_time >= t3 && m_time < t4)
                {
                    double t = 1.0 * (m_time - t3) / (t4 - t3);
                    alpha = (int)(s->m_effects[k]->param[1] * (1 - t) + s->m_effects[k]->param[2] * t);
                }
                else if(m_time >= t4) alpha = s->m_effects[k]->param[2];
            }
            break;
            case EF_BANNER: // Banner;delay=param[0][;leftoright=param[1];fadeawaywidth=param[2]]
            {
                int left = s->m_relativeTo == 1 ? m_vidrect.left : 0,
                    right = s->m_relativeTo == 1 ? m_vidrect.right : m_size.cx;

                r.left = !!s->m_effects[k]->param[1]
                         ? (left/*marginRect.left*/ - spaceNeeded.cx) + (int)(m_time * 8.0 / s->m_effects[k]->param[0])
                         : (right /*- marginRect.right*/) - (int)(m_time * 8.0 / s->m_effects[k]->param[0]);

                r.right = r.left + spaceNeeded.cx;

                clipRect &= CRect(left >> 3, clipRect.top, right >> 3, clipRect.bottom);

                fPosOverride = true;
            }
            break;
            case EF_SCROLL: // Scroll up/down(toptobottom=param[3]);top=param[0];bottom=param[1];delay=param[2][;fadeawayheight=param[4]]
            {
                r.top = !!s->m_effects[k]->param[3]
                        ? s->m_effects[k]->param[0] + (int)(m_time * 8.0 / s->m_effects[k]->param[2]) - spaceNeeded.cy
                        : s->m_effects[k]->param[1] - (int)(m_time * 8.0 / s->m_effects[k]->param[2]);

                r.bottom = r.top + spaceNeeded.cy;

                CRect cr(0, (s->m_effects[k]->param[0] + 4) >> 3, tspd.w, (s->m_effects[k]->param[1] + 4) >> 3);

                if(s->m_relativeTo == 1)
                    r.top += m_vidrect.top,
                             r.bottom += m_vidrect.top,
                                         cr.top += m_vidrect.top >> 3,
                                                   cr.bottom += m_vidrect.top >> 3;

                clipRect &= cr;

                fPosOverride = true;
            }
            /* Upstream VSFilterMod inserted the case below in front of the
             * break that ended this one, so a Scroll effect fell through
             * into it and turned the line's \clip into a "moving" vector
             * clip placed at the scroll parameters. */
            break;
#ifdef _VSMOD // patch m006. moveable vector clip
            case EF_VECTCLP:
            {
                CPoint vcpos1(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
                CPoint vcpos2(s->m_effects[k]->param[2], s->m_effects[k]->param[3]);
                int to1 = s->m_effects[k]->t[0];
                int to2 = s->m_effects[k]->t[1];

                if(to2 < to1)
                {
                    int to = to1;
                    to1 = to2;
                    to2 = to;
                }

                if(to1 <= 0 && to2 <= 0)
                {
                    to1 = 0;
                    to2 = m_delay;
                }

                if(m_time <= to1) mod_vc.pos = vcpos1;
                else if(to1 == to2) mod_vc.pos = vcpos1;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                else if(to1 < m_time && m_time < to2)
                {
                    double t = 1.0 * (m_time - to1) / (to2 - to1);
                    mod_vc.pos.x = (int)((1 - t) * vcpos1.x + t * vcpos2.x);
                    mod_vc.pos.y = (int)((1 - t) * vcpos1.y + t * vcpos2.y);
                }
                else mod_vc.pos = vcpos2;
                mod_vc.enable = true;
                mod_vc.isInverse = s->m_pClipper ? s->m_pClipper->m_inverse : false;
            }
#endif
            break;
            default:
                break;
            }
        }

        if(!fPosOverride && !fOrgOverride && !s->m_fAnimated)
            r = m_sla.AllocRect(s, segment, entry, stse.layer, m_collisions);

        CPoint org;
        org.x = (s->m_scrAlignment % 3) == 1 ? r.left : (s->m_scrAlignment % 3) == 2 ? r.CenterPoint().x : r.right;
        org.y = s->m_scrAlignment <= 3 ? r.bottom : s->m_scrAlignment <= 6 ? r.CenterPoint().y : r.top;

        if(!fOrgOverride) org2 = org;

        CPoint p, p2(0, r.top);

        POSITION pos;

        p = p2;

#ifdef _VSMOD // patch m014. feathered clip
        /* The nominal rectangle is what the user wrote in \clip; the painting
         * rectangle is inflated by the feather width so the ramp outside the
         * nominal edge has something to fade. Vector clips feather their mask
         * instead and never come through here. */
        CRect nominalClipRect = clipRect;
        const double clipFeather = s->m_clipFeather;
        const bool featherRectClip = clipFeather > 0 && s->m_clipExplicit && !s->m_pClipper;
        if(featherRectClip && !s->m_clipInverse)
        {
            const int finfl = (int)(clipFeather + 0.5) + 1;
            clipRect.left -= finfl;
            clipRect.top -= finfl;
            clipRect.right += finfl;
            clipRect.bottom += finfl;
        }
#endif

        // Rectangles for inverse clip
        CRect iclipRect[4];
        iclipRect[0] = CRect(0, 0, tspd.w, clipRect.top);
        iclipRect[1] = CRect(0, clipRect.top, clipRect.left, clipRect.bottom);
        iclipRect[2] = CRect(clipRect.right, clipRect.top, tspd.w, clipRect.bottom);
        iclipRect[3] = CRect(0, clipRect.bottom, tspd.w, tspd.h);

#ifdef _VSMOD // patch m012. glow
        std::vector<GlowPass> glowPasses;
        CollectGlowPasses(s, glowPasses);
        for(auto glowIt = glowPasses.rbegin(); glowIt != glowPasses.rend(); ++glowIt)
        {
            const int glowRadius = glowIt->radius;
            const int glowStrengthQ8 = glowIt->strengthQ8;
            const int blurRadius = (std::min)(glowRadius, (std::max)(tspd.w, tspd.h));
            GlowCanvas glowCanvas(tspd.w, tspd.h);
            CRect glowSourceBBox(0, 0, 0, 0);

            const auto paintGlowSource = [&](CRect& sourceClipRect)
            {
                p = p2;
                pos = s->GetHeadPosition();
                while(pos)
                {
                    CLine* l = s->GetNext(pos);

                    p.x = (s->m_scrAlignment % 3) == 1 ? org.x
                          : (s->m_scrAlignment % 3) == 0 ? org.x - l->m_width
                          :                                  org.x - (l->m_width / 2);

                    /* The glow source is clipped before blurring. Otherwise a
                     * word hidden by \clip could still cast light into the
                     * visible part of the frame. */
                    glowSourceBBox |= l->PaintOutline(glowCanvas.desc, sourceClipRect, pAlphaMask, p, org2, m_time, alpha, mod_vc, rt, PaintPassFilter(glowRadius, glowStrengthQ8));
                    glowSourceBBox |= l->PaintBody(glowCanvas.desc, sourceClipRect, pAlphaMask, p, org2, m_time, alpha, mod_vc, rt, PaintPassFilter(glowRadius, glowStrengthQ8));

                    p.y += l->m_ascent + l->m_descent;
                }
            };

            if(s->m_clipInverse)
            {
                for(int iclip = 0; iclip < 4; iclip++) paintGlowSource(iclipRect[iclip]);
            }
            else
            {
                paintGlowSource(clipRect);
            }

            if(glowSourceBBox.IsRectEmpty()) continue;

            glowCanvas.ConvertToPremultipliedAlpha();

            CRect glowRect(glowSourceBBox.left - blurRadius - 1, glowSourceBBox.top - blurRadius - 1,
                           glowSourceBBox.right + blurRadius + 1, glowSourceBBox.bottom + blurRadius + 1);
            if(glowRect.left < 0) glowRect.left = 0;
            if(glowRect.top < 0) glowRect.top = 0;
            if(glowRect.right > tspd.w) glowRect.right = tspd.w;
            if(glowRect.bottom > tspd.h) glowRect.bottom = tspd.h;
            if(glowRect.IsRectEmpty()) continue;

            GlowPlane16 plane;
            plane.Setup(glowRect.left, glowRect.top,
                        glowRect.right - glowRect.left, glowRect.bottom - glowRect.top);

            /* Bloom-style mixture of three Gaussian scales: a hot tight core,
             * a mid component and a wide soft halo. Equal radii are merged so
             * small glows do not pay for redundant passes. */
            int scaleCount = 0;
            struct { int radius; int weight; } scales[3];
            const int scaleRadius[3] = {blurRadius / 6, blurRadius * 2 / 5, blurRadius};
            /* Exposure: strength scales the linear emission before tone
             * mapping, matching AE-style exposure behaviour. Base weights
             * 0.7/0.45/0.35 sum to 1.5, so the core can rise well above
             * intensity 1.0 and the tone mapper shapes it into a hot core
             * with a long soft falloff instead of a flat cloud. */
            const int scaleWeight[3] = {(179 * glowStrengthQ8 + 128) >> 8,
                                        (115 * glowStrengthQ8 + 128) >> 8,
                                        (90 * glowStrengthQ8 + 128) >> 8};
            for(int i = 0; i < 3; i++)
            {
                const int radius = (std::max)(1, scaleRadius[i]);
                int found = -1;
                for(int j = 0; j < scaleCount; j++)
                    if(scales[j].radius == radius) found = j;
                if(found >= 0) scales[found].weight += scaleWeight[i];
                else
                {
                    scales[scaleCount].radius = radius;
                    scales[scaleCount].weight = scaleWeight[i];
                    scaleCount++;
                }
            }

            for(int i = 0; i < scaleCount; i++)
            {
                plane.LoadSource(glowCanvas);
                BlurGlowPlane16(plane, scales[i].radius);
                plane.Accumulate(scales[i].weight);
            }

            if(s->m_clipInverse)
            {
                for(int iclip = 0; iclip < 4; iclip++)
                {
                    const CRect compositeRect = IntersectGlowRect(glowRect, iclipRect[iclip]);
                    if(!compositeRect.IsRectEmpty())
                    {
                        CompositeGlowPlane(plane, tspd, compositeRect, 256, pAlphaMask, mod_vc);
                        tbbox |= compositeRect;
                    }
                }
            }
            else
            {
                const CRect compositeRect = IntersectGlowRect(glowRect, clipRect);
                if(!compositeRect.IsRectEmpty())
                {
                    CompositeGlowPlane(plane, tspd, compositeRect, 256, pAlphaMask, mod_vc);
                    tbbox |= compositeRect;
                }
            }
        }

        p = p2;
#endif

        pos = s->GetHeadPosition();
        while(pos)
        {
            CLine* l = s->GetNext(pos);

            p.x = (s->m_scrAlignment % 3) == 1 ? org.x
                  : (s->m_scrAlignment % 3) == 0 ? org.x - l->m_width
                  :							   org.x - (l->m_width / 2);

#ifdef _VSMOD // patch m006. moveable vector clip
            if(s->m_clipInverse)
            {
                tbbox |= l->PaintShadow(tspd, iclipRect[0], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintShadow(tspd, iclipRect[1], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintShadow(tspd, iclipRect[2], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintShadow(tspd, iclipRect[3], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
            }
            else
            {
                tbbox |= l->PaintShadow(tspd, clipRect, pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
            }
#else
            if(s->m_clipInverse)
            {
                tbbox |= l->PaintShadow(tspd, iclipRect[0], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintShadow(tspd, iclipRect[1], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintShadow(tspd, iclipRect[2], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintShadow(tspd, iclipRect[3], pAlphaMask, p, org2, m_time, alpha);
            }
            else
            {
                tbbox |= l->PaintShadow(tspd, clipRect, pAlphaMask, p, org2, m_time, alpha);
            }
#endif
            p.y += l->m_ascent + l->m_descent;
        }

        p = p2;

        pos = s->GetHeadPosition();
        while(pos)
        {
            CLine* l = s->GetNext(pos);

            p.x = (s->m_scrAlignment % 3) == 1 ? org.x
                  : (s->m_scrAlignment % 3) == 0 ? org.x - l->m_width
                  :							   org.x - (l->m_width / 2);

#ifdef _VSMOD // patch m006. moveable vector clip
            if(s->m_clipInverse)
            {
                tbbox |= l->PaintOutline(tspd, iclipRect[0], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintOutline(tspd, iclipRect[1], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintOutline(tspd, iclipRect[2], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintOutline(tspd, iclipRect[3], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
            }
            else
            {
                tbbox |= l->PaintOutline(tspd, clipRect, pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
            }
#else
            if(s->m_clipInverse)
            {
                tbbox |= l->PaintOutline(tspd, iclipRect[0], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintOutline(tspd, iclipRect[1], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintOutline(tspd, iclipRect[2], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintOutline(tspd, iclipRect[3], pAlphaMask, p, org2, m_time, alpha);
            }
            else
            {
                tbbox |= l->PaintOutline(tspd, clipRect, pAlphaMask, p, org2, m_time, alpha);
            }
#endif
            p.y += l->m_ascent + l->m_descent;
        }

        p = p2;

        pos = s->GetHeadPosition();
        while(pos)
        {
            CLine* l = s->GetNext(pos);

            p.x = (s->m_scrAlignment % 3) == 1 ? org.x
                  : (s->m_scrAlignment % 3) == 0 ? org.x - l->m_width
                  :							   org.x - (l->m_width / 2);

#ifdef _VSMOD // patch m006. moveable vector clip
            if(s->m_clipInverse)
            {
                tbbox |= l->PaintBody(tspd, iclipRect[0], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintBody(tspd, iclipRect[1], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintBody(tspd, iclipRect[2], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
                tbbox |= l->PaintBody(tspd, iclipRect[3], pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
            }
            else
            {
                tbbox |= l->PaintBody(tspd, clipRect, pAlphaMask, p, org2, m_time, alpha, mod_vc, rt);
            }
#else
            if(s->m_clipInverse)
            {
                tbbox |= l->PaintBody(tspd, iclipRect[0], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintBody(tspd, iclipRect[1], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintBody(tspd, iclipRect[2], pAlphaMask, p, org2, m_time, alpha);
                tbbox |= l->PaintBody(tspd, iclipRect[3], pAlphaMask, p, org2, m_time, alpha);
            }
            else
            {
                tbbox |= l->PaintBody(tspd, clipRect, pAlphaMask, p, org2, m_time, alpha);
            }
#endif
            p.y += l->m_ascent + l->m_descent;
        }

#ifdef _VSMOD // patch m014. inner glow
        /* Inner glow is part of the glyph appearance, so it composites after
         * the body painting and before the bitmap post-effects. The emission
         * is coverage x blur(1 - coverage): light bleeding inward from the
         * glyph edges. */
        std::vector<IGlowPass> iglowPasses;
        CollectIGlowPasses(s, iglowPasses);
        for(size_t ip = 0; ip < iglowPasses.size(); ip++)
        {
            const IGlowPass& pass = iglowPasses[ip];
            GlowCanvas iglowCanvas(tspd.w, tspd.h);
            CRect iglowCoverageBox(0, 0, 0, 0);

            const auto paintIGlowSource = [&](CRect& sourceClipRect)
            {
                p = p2;
                pos = s->GetHeadPosition();
                while(pos)
                {
                    CLine* l = s->GetNext(pos);

                    p.x = (s->m_scrAlignment % 3) == 1 ? org.x
                          : (s->m_scrAlignment % 3) == 0 ? org.x - l->m_width
                          :                                  org.x - (l->m_width / 2);

                    iglowCoverageBox |= l->PaintOutline(iglowCanvas.desc, sourceClipRect, pAlphaMask, p, org2, m_time, alpha, mod_vc, rt, PaintPassFilter(pass.radius, pass.strengthQ8, pass.color, pass.alphaKey));
                    iglowCoverageBox |= l->PaintBody(iglowCanvas.desc, sourceClipRect, pAlphaMask, p, org2, m_time, alpha, mod_vc, rt, PaintPassFilter(pass.radius, pass.strengthQ8, pass.color, pass.alphaKey));

                    p.y += l->m_ascent + l->m_descent;
                }
            };

            if(s->m_clipInverse)
            {
                for(int iclip = 0; iclip < 4; iclip++) paintIGlowSource(iclipRect[iclip]);
            }
            else
            {
                paintIGlowSource(clipRect);
            }

            if(iglowCoverageBox.IsRectEmpty()) continue;

            const int blurRadius = (std::min)(pass.radius, (std::max)(tspd.w, tspd.h));
            /* The blur support must stay inside the region of interest for
             * every covered pixel, otherwise the zero padding beyond it would
             * darken the edge light. */
            CRect iglowRect(iglowCoverageBox.left - blurRadius - 1, iglowCoverageBox.top - blurRadius - 1,
                            iglowCoverageBox.right + blurRadius + 1, iglowCoverageBox.bottom + blurRadius + 1);
            if(iglowRect.left < 0) iglowRect.left = 0;
            if(iglowRect.top < 0) iglowRect.top = 0;
            if(iglowRect.right > tspd.w) iglowRect.right = tspd.w;
            if(iglowRect.bottom > tspd.h) iglowRect.bottom = tspd.h;
            if(iglowRect.IsRectEmpty()) continue;

            GlowPlane16 plane;
            plane.Setup(iglowRect.left, iglowRect.top,
                        iglowRect.right - iglowRect.left, iglowRect.bottom - iglowRect.top);
            LoadInvertedCoverage16(plane, iglowCanvas);
            BlurGlowPlane16(plane, (std::max)(1, blurRadius));

            if(s->m_clipInverse)
            {
                for(int iclip = 0; iclip < 4; iclip++)
                {
                    const CRect compositeRect = IntersectGlowRect(iglowCoverageBox, iclipRect[iclip]);
                    if(!compositeRect.IsRectEmpty())
                        CompositeInnerGlowPlane(plane, iglowCanvas, tspd, compositeRect, pass.strengthQ8, pass.color, pass.alphaKey, pAlphaMask, mod_vc);
                }
            }
            else
            {
                const CRect compositeRect = IntersectGlowRect(iglowCoverageBox, clipRect);
                if(!compositeRect.IsRectEmpty())
                    CompositeInnerGlowPlane(plane, iglowCanvas, tspd, compositeRect, pass.strengthQ8, pass.color, pass.alphaKey, pAlphaMask, mod_vc);
            }
        }

        p = p2;
#endif

#ifdef _VSMOD // patch m014. bevel and directional blur bitmap post-effects
        /* Like \rgb these apply to the whole painted sentence; the largest
         * magnitude wins when words disagree, and the winner's secondary
         * parameters (angle, strength) come along. */
        double bevelSize = 0, bevelStrength = 1;
        double mblurLength = 0, mblurAngle = 0;
        POSITION fxlinepos = s->GetHeadPosition();
        while(fxlinepos)
        {
            CLine* l = s->GetNext(fxlinepos);
            POSITION fxwordpos = l->GetHeadPosition();
            while(fxwordpos)
            {
                const CWord* w = l->GetNext(fxwordpos);
                if(fabs(w->m_style.mod_bevelSize) > fabs(bevelSize))
                {
                    bevelSize = w->m_style.mod_bevelSize;
                    bevelStrength = w->m_style.mod_bevelStrength;
                }
                if(fabs(w->m_style.mod_mblurLength) > fabs(mblurLength))
                {
                    mblurLength = w->m_style.mod_mblurLength;
                    mblurAngle = w->m_style.mod_mblurAngle;
                }
            }
        }
        if(bevelSize > 0 && !tbbox.IsRectEmpty())
        {
            ApplyBevel(tspd, tbbox, (int)(bevelSize + 0.5), (int)(bevelStrength * 256 + 0.5));
        }
        if(mblurLength > 0 && !tbbox.IsRectEmpty())
        {
            ApplyMotionBlur(tspd, tbbox, mblurLength, mblurAngle);
            /* the smear spreads the content box along the blur direction */
            const double mrad = mblurAngle * (3.14159265358979323846 / 180.0);
            const int mex = (int)(fabs(vsfm::DetCos(mrad)) * mblurLength / 2 + 1.5);
            const int mey = (int)(fabs(vsfm::DetSin(mrad)) * mblurLength / 2 + 1.5);
            tbbox.left -= mex;
            tbbox.right += mex;
            tbbox.top -= mey;
            tbbox.bottom += mey;
            if(tbbox.left < 0) tbbox.left = 0;
            if(tbbox.top < 0) tbbox.top = 0;
            if(tbbox.right > tspd.w) tbbox.right = tspd.w;
            if(tbbox.bottom > tspd.h) tbbox.bottom = tspd.h;
        }
#endif

#ifdef _VSMOD // patch m013. RGB split
        /* Chromatic aberration: shift the red and blue channels of the painted
         * sentence in opposite directions. This is mathematically identical to
         * drawing per-channel glyphs with offsets, but needs no changes to the
         * painting pipeline. Applies to the whole sentence; the largest
         * magnitude wins when words disagree. */
        double rgbSplit = 0;
        POSITION rgblinepos = s->GetHeadPosition();
        while(rgblinepos)
        {
            CLine* l = s->GetNext(rgblinepos);
            POSITION rgbwordpos = l->GetHeadPosition();
            while(rgbwordpos)
            {
                const double d = l->GetNext(rgbwordpos)->m_style.mod_rgbSplit;
                if(fabs(d) > fabs(rgbSplit)) rgbSplit = d;
            }
        }
        int rgbShift = (int)(rgbSplit >= 0 ? rgbSplit + 0.5 : rgbSplit - 0.5);
        if(rgbShift > 4096) rgbShift = 4096;
        else if(rgbShift < -4096) rgbShift = -4096;
        if(rgbShift && !tbbox.IsRectEmpty())
        {
            const int ad = rgbShift < 0 ? -rgbShift : rgbShift;
            CRect rr(tbbox.left - ad, tbbox.top, tbbox.right + ad, tbbox.bottom);
            if(rr.left < 0) rr.left = 0;
            if(rr.right > tspd.w) rr.right = tspd.w;
            if(rr.bottom > tspd.h) rr.bottom = tspd.h;
            if(rr.top < 0) rr.top = 0;
            const int rw = rr.right - rr.left;
            const int rh = rr.bottom - rr.top;
            if(rw > 0 && rh > 0)
            {
                std::vector<unsigned int> backup((size_t)rw * rh);
                for(int y = 0; y < rh; y++)
                {
                    const unsigned int* srow = (const unsigned int*)((const BYTE*)tspd.bits + (rr.top + y) * tspd.pitch) + rr.left;
                    memcpy(&backup[(size_t)y * rw], srow, rw * 4);
                }
                for(int y = 0; y < rh; y++)
                {
                    unsigned int* drow = (unsigned int*)((BYTE*)tspd.bits + (rr.top + y) * tspd.pitch) + rr.left;
                    for(int x = 0; x < rw; x++)
                    {
                        /* BGRA byte order: blue=0, green=1, red=2, alpha=3.
                         * Red moves by +shift, blue by -shift. */
                        int xr = x - rgbShift;
                        int xb = x + rgbShift;
                        if(xr < 0) xr = 0; else if(xr >= rw) xr = rw - 1;
                        if(xb < 0) xb = 0; else if(xb >= rw) xb = rw - 1;
                        const unsigned int pr = backup[(size_t)y * rw + xr];
                        const unsigned int pb = backup[(size_t)y * rw + xb];
                        unsigned int px = backup[(size_t)y * rw + x];
                        px = (px & 0xff00ff00u)
                             | (pb & 0x000000ffu)
                             | (pr & 0x00ff0000u);
                        drow[x] = px;
                    }
                }
                tbbox.left -= ad;
                tbbox.right += ad;
                if(tbbox.left < 0) tbbox.left = 0;
                if(tbbox.right > tspd.w) tbbox.right = tspd.w;
            }
        }
#endif

#ifdef _VSMOD // patch m014. feathered rectangular clip
        /* Last of the post-effects: the feather ramp must apply cleanly to the
         * final pixels, including the glow and any channel-shifted fringes. */
        if(featherRectClip)
            ApplyClipFeather(tspd, nominalClipRect, clipFeather, s->m_clipInverse);
#endif

#ifdef _VSMOD // patch m016. track matte
        if(myMatteState)
        {
            /* the canvas accumulates every matte event on this layer; the
             * extraction is a pure function of the canvas, so re-extracting
             * after each event keeps the union coverage current */
            ExtractMatteCoverage(myMatteState->pixels, myMatteState->coverage, myMatteState->inverse);
            myMatteState->hasCoverage = true;
        }
        else if(consumeMatte && !evtPixels.empty())
        {
            CompositeMattedCanvas(evtPixels, consumeMatte->coverage, spd, localBBox);
            CRect clipped(localBBox);
            if(clipped.left < 0) clipped.left = 0;
            if(clipped.top < 0) clipped.top = 0;
            if(clipped.right > spd.w) clipped.right = spd.w;
            if(clipped.bottom > spd.h) clipped.bottom = spd.h;
            bbox2 |= clipped;
        }
#endif

        //delete s;
    }

    bbox = bbox2;

    return (subs.GetCount() && !bbox2.IsRectEmpty()) ? S_OK : S_FALSE;
}
