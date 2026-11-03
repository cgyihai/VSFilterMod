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
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "Rasterizer.h"
#include "SeparableFilter.h"

#ifndef _MAX	/* avoid collision with common (nonconforming) macros */
#define _MAX	(max)
#define _MIN	(min)
#define _IMPL_MAX max
#define _IMPL_MIN min
#else
#define _IMPL_MAX _MAX
#define _IMPL_MIN _MIN
#endif

int Rasterizer::getOverlayWidth()
{
    return mOverlayWidth * 8;
}

Rasterizer::Rasterizer()
    : fFirstSet(false), mpPathTypes(NULL), mpPathPoints(NULL), mPathPoints(0),
      mWidth(0), mHeight(0), mWideBorder(0), mpEdgeBuffer(NULL), mEdgeHeapSize(0),
      mEdgeNext(0), mpScanBuffer(NULL), mpOverlayBuffer(NULL)
{
    mOverlayWidth = mOverlayHeight = 0;
    mPathOffsetX = mPathOffsetY = 0;
    mOffsetX = mOffsetY = 0;
}

Rasterizer::~Rasterizer()
{
    _TrashPath();
    _TrashOverlay();
}

void Rasterizer::_TrashPath()
{
    free(mpPathTypes);
    free(mpPathPoints);
    mpPathTypes = NULL;
    mpPathPoints = NULL;
    mPathPoints = 0;
}

void Rasterizer::_TrashOverlay()
{
    delete [] mpOverlayBuffer;
    mpOverlayBuffer = NULL;
}

void Rasterizer::_ReallocEdgeBuffer(int edges)
{
    mEdgeHeapSize = edges;
    mpEdgeBuffer = (Edge*)realloc(mpEdgeBuffer, sizeof(Edge) * edges);
}

void Rasterizer::_EvaluateBezier(int ptbase, bool fBSpline)
{
    const POINT* pt0 = mpPathPoints + ptbase;
    const POINT* pt1 = mpPathPoints + ptbase + 1;
    const POINT* pt2 = mpPathPoints + ptbase + 2;
    const POINT* pt3 = mpPathPoints + ptbase + 3;

    double x0 = pt0->x;
    double x1 = pt1->x;
    double x2 = pt2->x;
    double x3 = pt3->x;
    double y0 = pt0->y;
    double y1 = pt1->y;
    double y2 = pt2->y;
    double y3 = pt3->y;

    double cx3, cx2, cx1, cx0, cy3, cy2, cy1, cy0;

    if(fBSpline)
    {
        // 1   [-1 +3 -3 +1]
        // - * [+3 -6 +3  0]
        // 6   [-3  0 +3  0]
        //	   [+1 +4 +1  0]

        double _1div6 = 1.0 / 6.0;

        cx3 = _1div6 * (-  x0 + 3 * x1 - 3 * x2 + x3);
        cx2 = _1div6 * (3 * x0 - 6 * x1 + 3 * x2);
        cx1 = _1div6 * (-3 * x0	   + 3 * x2);
        cx0 = _1div6 * (x0 + 4 * x1 + 1 * x2);

        cy3 = _1div6 * (-  y0 + 3 * y1 - 3 * y2 + y3);
        cy2 = _1div6 * (3 * y0 - 6 * y1 + 3 * y2);
        cy1 = _1div6 * (-3 * y0     + 3 * y2);
        cy0 = _1div6 * (y0 + 4 * y1 + 1 * y2);
    }
    else // bezier
    {
        // [-1 +3 -3 +1]
        // [+3 -6 +3  0]
        // [-3 +3  0  0]
        // [+1  0  0  0]

        cx3 = -  x0 + 3 * x1 - 3 * x2 + x3;
        cx2 =  3 * x0 - 6 * x1 + 3 * x2;
        cx1 = -3 * x0 + 3 * x1;
        cx0 =    x0;

        cy3 = -  y0 + 3 * y1 - 3 * y2 + y3;
        cy2 =  3 * y0 - 6 * y1 + 3 * y2;
        cy1 = -3 * y0 + 3 * y1;
        cy0 =    y0;
    }

    //
    // This equation is from Graphics Gems I.
    //
    // The idea is that since we're approximating a cubic curve with lines,
    // any error we incur is due to the curvature of the line, which we can
    // estimate by calculating the maximum acceleration of the curve.  For
    // a cubic, the acceleration (second derivative) is a line, meaning that
    // the absolute maximum acceleration must occur at either the beginning
    // (|c2|) or the end (|c2+c3|).  Our bounds here are a little more
    // conservative than that, but that's okay.
    //
    // If the acceleration of the parametric formula is zero (c2 = c3 = 0),
    // that component of the curve is linear and does not incur any error.
    // If a=0 for both X and Y, the curve is a line segment and we can
    // use a step size of 1.

    double maxaccel1 = fabs(2 * cy2) + fabs(6 * cy3);
    double maxaccel2 = fabs(2 * cx2) + fabs(6 * cx3);

    double maxaccel = maxaccel1 > maxaccel2 ? maxaccel1 : maxaccel2;
    double h = 1.0;

    if(maxaccel > 8.0) h = sqrt(8.0 / maxaccel);

    if(!fFirstSet)
    {
        firstp.x = (LONG)cx0;
        firstp.y = (LONG)cy0;
        lastp = firstp;
        fFirstSet = true;
    }

    for(double t = 0; t < 1.0; t += h)
    {
        double x = cx0 + t * (cx1 + t * (cx2 + t * cx3));
        double y = cy0 + t * (cy1 + t * (cy2 + t * cy3));
        _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
    }

    double x = cx0 + cx1 + cx2 + cx3;
    double y = cy0 + cy1 + cy2 + cy3;
    _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
}

void Rasterizer::_EvaluateLine(int pt1idx, int pt2idx)
{
    const POINT* pt1 = mpPathPoints + pt1idx;
    const POINT* pt2 = mpPathPoints + pt2idx;

    _EvaluateLine(pt1->x, pt1->y, pt2->x, pt2->y);
}

void Rasterizer::_EvaluateLine(int x0, int y0, int x1, int y1)
{
    if(lastp.x != x0 || lastp.y != y0)
    {
        _EvaluateLine(lastp.x, lastp.y, x0, y0);
    }

    if(!fFirstSet)
    {
        firstp.x = x0;
        firstp.y = y0;
        fFirstSet = true;
    }
    lastp.x = x1;
    lastp.y = y1;

    if(y1 > y0)	// down
    {
        __int64 xacc = (__int64)x0 << 13;

        // prestep y0 down

        int dy = y1 - y0;
        int y = ((y0 + 3)&~7) + 4;
        int iy = y >> 3;

        y1 = (y1 - 5) >> 3;

        if(iy <= y1)
        {
            __int64 invslope = ((__int64)(x1 - x0) * 65536) / dy;

            while(mEdgeNext + y1 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize * 2);

            xacc += (invslope * (y - y0)) >> 3;

            while(iy <= y1)
            {
                int ix = (int)((xacc + 32768) >> 16);

                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix * 2 + 1;

                mpScanBuffer[iy] = mEdgeNext++;

                ++iy;
                xacc += invslope;
            }
        }
    }
    else if(y1 < y0) // up
    {
        __int64 xacc = (__int64)x1 << 13;

        // prestep y1 down

        int dy = y0 - y1;
        int y = ((y1 + 3)&~7) + 4;
        int iy = y >> 3;

        y0 = (y0 - 5) >> 3;

        if(iy <= y0)
        {
            __int64 invslope = ((__int64)(x0 - x1) * 65536) / dy;

            while(mEdgeNext + y0 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize * 2);

            xacc += (invslope * (y - y1)) >> 3;

            while(iy <= y0)
            {
                int ix = (int)((xacc + 32768) >> 16);

                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix * 2;

                mpScanBuffer[iy] = mEdgeNext++;

                ++iy;
                xacc += invslope;
            }
        }
    }
}

/* Installs a complete glyph path, replacing whatever was there.
 * (Was BeginPath + TextOutW + EndPath + GetPath.) */
bool Rasterizer::SetPath(const vsfm::GlyphPath& path)
{
    _TrashPath();

    const size_t n = path.points.size();
    if (n == 0) return true;

    mpPathTypes = (BYTE*)malloc(sizeof(BYTE) * n);
    mpPathPoints = (POINT*)malloc(sizeof(POINT) * n);

    if (!mpPathTypes || !mpPathPoints) {
        _TrashPath();
        return false;
    }

    memcpy(mpPathPoints, path.points.data(), sizeof(POINT) * n);
    memcpy(mpPathTypes, path.types.data(), sizeof(BYTE) * n);
    mPathPoints = n;

    return true;
}

/* Appends a glyph path offset by (dx, dy), optionally clearing first.
 * (Was PartialBeginPath + TextOutW + PartialEndPath.) */
bool Rasterizer::AppendPath(const vsfm::GlyphPath& path, long dx, long dy, bool bClearPath)
{
    if (bClearPath)
        _TrashPath();

    const size_t n = path.points.size();
    if (n == 0) return true;

    const size_t total = mPathPoints + n;

    BYTE* pNewTypes = (BYTE*)realloc(mpPathTypes, total * sizeof(BYTE));
    POINT* pNewPoints = (POINT*)realloc(mpPathPoints, total * sizeof(POINT));

    if (pNewTypes) mpPathTypes = pNewTypes;
    if (pNewPoints) mpPathPoints = pNewPoints;

    if (!pNewTypes || !pNewPoints)
        return false;

    for (size_t i = 0; i < n; ++i) {
        mpPathPoints[mPathPoints + i].x = path.points[i].x + dx;
        mpPathPoints[mPathPoints + i].y = path.points[i].y + dy;
        mpPathTypes[mPathPoints + i] = path.types[i];
    }

    mPathPoints = total;
    return true;
}

/* Limits on what is drawn, in eighths of a pixel; see ScanConvert. */
static const int64_t kMaxShapeExtent = (int64_t)1 << 20;
static const int64_t kMaxShapeArea = (int64_t)1 << 34;

bool Rasterizer::ScanConvert()
{
    size_t lastmoveto = -1;
    size_t i;

    // Drop any outlines we may have.

    mOutline.clear();
    mWideOutline.clear();
    mWideBorder = 0;

    // Determine bounding box

    if(!mPathPoints)
    {
        mPathOffsetX = mPathOffsetY = 0;
        mWidth = mHeight = 0;
        return 0;
    }

    /* 64-bit on every platform: ptrdiff_t is 32 bits on 32-bit Windows, where
     * the extent of a path with extreme coordinates would overflow. */
    int64_t minx = INT_MAX;
    int64_t miny = INT_MAX;
    int64_t maxx = INT_MIN;
    int64_t maxy = INT_MIN;

    for(i = 0; i < mPathPoints; ++i)
    {
        int64_t ix = mpPathPoints[i].x;
        int64_t iy = mpPathPoints[i].y;

        if(ix < minx) minx = ix;
        if(ix > maxx) maxx = ix;
        if(iy < miny) miny = iy;
        if(iy > maxy) maxy = iy;
    }

    minx = (minx >> 3) & ~7;
    miny = (miny >> 3) & ~7;
    maxx = (maxx + 7) >> 3;
    maxy = (maxy + 7) >> 3;

    for(i = 0; i < mPathPoints; ++i)
    {
        mpPathPoints[i].x -= minx * 8;
        mpPathPoints[i].y -= miny * 8;
    }

    if(minx > maxx || miny > maxy)
    {
        mWidth = mHeight = 0;
        mPathOffsetX = mPathOffsetY = 0;
        _TrashPath();
        return true;
    }

    /* A shape no screen could show -- "m -2147483648 0 l 400 0 ..." or a
     * glyph stretched by \fscy10000000 -- would otherwise be scan-converted
     * and filled in full: tens of millions of spans, gigabytes of memory and
     * minutes of work for one frame. Anything wider or taller than 2^17
     * pixels (thirty times an 8K frame), or larger than 2^28 pixels in area
     * (thirty 4K frames), is not drawn. Width and height here are in eighths
     * of a pixel. */
    const int64_t width = maxx + 1 - minx, height = maxy + 1 - miny;
    if(width > kMaxShapeExtent || height > kMaxShapeExtent || width * height > kMaxShapeArea)
    {
        mWidth = mHeight = 0;
        mPathOffsetX = mPathOffsetY = 0;
        _TrashPath();
        return true;
    }

    mWidth = (int)width;
    mHeight = (int)height;

    mPathOffsetX = (int)minx;
    mPathOffsetY = (int)miny;

    // Initialize edge buffer.  We use edge 0 as a sentinel.

    mEdgeNext = 1;
    mEdgeHeapSize = 2048;
    mpEdgeBuffer = (Edge*)malloc(sizeof(Edge) * mEdgeHeapSize);

    // Initialize scanline list.

    mpScanBuffer = DNew size_t[mHeight];
    memset(mpScanBuffer, 0, mHeight * sizeof(size_t));

    // Scan convert the outline.  Yuck, Bezier curves....

    // Unfortunately, Windows 95/98 GDI has a bad habit of giving us text
    // paths with all but the first figure left open, so we can't rely
    // on the PT_CLOSEFIGURE flag being used appropriately.

    fFirstSet = false;
    firstp.x = firstp.y = 0;
    lastp.x = lastp.y = 0;

    for(i = 0; i < mPathPoints; ++i)
    {
        BYTE t = mpPathTypes[i] & ~PT_CLOSEFIGURE;

        switch(t)
        {
        case PT_MOVETO:
            if(lastmoveto >= 0 && firstp != lastp)
                _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);
            lastmoveto = i;
            fFirstSet = false;
            lastp = mpPathPoints[i];
            break;
        case PT_MOVETONC:
            break;
        case PT_LINETO:
            if(mPathPoints - (i - 1) >= 2) _EvaluateLine(i - 1, i);
            break;
        case PT_BEZIERTO:
            if(mPathPoints - (i - 1) >= 4) _EvaluateBezier(i - 1, false);
            i += 2;
            break;
        case PT_BSPLINETO:
            if(mPathPoints - (i - 1) >= 4) _EvaluateBezier(i - 1, true);
            i += 2;
            break;
        case PT_BSPLINEPATCHTO:
            if(mPathPoints - (i - 3) >= 4) _EvaluateBezier(i - 3, true);
            break;
        }
    }

    if(lastmoveto >= 0 && firstp != lastp)
        _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);

    // Free the path since we don't need it anymore.

    _TrashPath();

    // Convert the edges to spans.  We couldn't do this before because some of
    // the regions may have winding numbers >+1 and it would have been a pain
    // to try to adjust the spans on the fly.  We use one heap to detangle
    // a scanline's worth of edges from the singly-linked lists, and another
    // to collect the actual scans.

    std::vector<int> heap;

    mOutline.reserve(mEdgeNext / 2);

    __int64 y = 0;

    for(y = 0; y < mHeight; ++y)
    {
        int count = 0;

        // Detangle scanline into edge heap.

        for(size_t ptr = (mpScanBuffer[y] & size_t(-1)); ptr; ptr = mpEdgeBuffer[ptr].next)
        {
            heap.push_back(mpEdgeBuffer[ptr].posandflag);
        }

        // Sort edge heap.  Note that we conveniently made the opening edges
        // one more than closing edges at the same spot, so we won't have any
        // problems with abutting spans.

        std::sort(heap.begin(), heap.end()/*begin() + heap.size()*/);

        // Process edges and add spans.  Since we only check for a non-zero
        // winding number, it doesn't matter which way the outlines go!

        std::vector<int>::iterator itX1 = heap.begin();
        std::vector<int>::iterator itX2 = heap.end(); // begin() + heap.size();

        size_t x1, x2;

        for(; itX1 != itX2; ++itX1)
        {
            size_t x = *itX1;

            if(!count)
                x1 = (x >> 1);

            if(x & 1)
                ++count;
            else
                --count;

            if(!count)
            {
                x2 = (x >> 1);

                if(x2 > x1)
                    mOutline.push_back(std::pair<__int64, __int64>((y << 32) + x1 + 0x4000000040000000LL, (y << 32) + x2 + 0x4000000040000000LL)); // G: damn Avery, this is evil! :)
            }
        }

        heap.clear();
    }

    // Dump the edge and scan buffers, since we no longer need them.

    free(mpEdgeBuffer);
    delete [] mpScanBuffer;

    // All done!

    return true;
}

using namespace std;

void Rasterizer::_OverlapRegion(tSpanBuffer& dst, tSpanBuffer& src, int dx, int dy)
{
    tSpanBuffer temp;

    temp.reserve(dst.size() + src.size());

    dst.swap(temp);

    tSpanBuffer::iterator itA = temp.begin();
    tSpanBuffer::iterator itAE = temp.end();
    tSpanBuffer::iterator itB = src.begin();
    tSpanBuffer::iterator itBE = src.end();

    // Don't worry -- even if dy<0 this will still work! // G: hehe, the evil twin :)

    uint64_t offset1 = ((__int64)dy * ((__int64)1 << 32)) - dx;
    uint64_t offset2 = ((__int64)dy * ((__int64)1 << 32)) + dx;

    while(itA != itAE && itB != itBE)
    {
        if((*itB).first + offset1 < (*itA).first)
        {
            // B span is earlier.  Use it.

            uint64_t x1 = (*itB).first + offset1;
            uint64_t x2 = (*itB).second + offset2;

            ++itB;

            // B spans don't overlap, so begin merge loop with A first.

            for(;;)
            {
                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.

                if(itA == itAE || (*itA).first > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itA++).second);
                }
                while(itA != itAE && (*itA).first <= x2);

                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.

                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itB++).second + offset2);
                }
                while(itB != itBE && (*itB).first + offset1 <= x2);
            }

            // Flush span.

            dst.push_back(tSpan(x1, x2));
        }
        else
        {
            // A span is earlier.  Use it.

            uint64_t x1 = (*itA).first;
            uint64_t x2 = (*itA).second;

            ++itA;

            // A spans don't overlap, so begin merge loop with B first.

            for(;;)
            {
                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.

                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itB++).second + offset2);
                }
                while(itB != itBE && (*itB).first + offset1 <= x2);

                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.

                if(itA == itAE || (*itA).first > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itA++).second);
                }
                while(itA != itAE && (*itA).first <= x2);
            }

            // Flush span.

            dst.push_back(tSpan(x1, x2));
        }
    }

    // Copy over leftover spans.

    while(itA != itAE)
        dst.push_back(*itA++);

    while(itB != itBE)
    {
        dst.push_back(tSpan((*itB).first + offset1, (*itB).second + offset2));
        ++itB;
    }
}

bool Rasterizer::CreateWidenedRegion(int rx, int ry)
{
    if(rx < 0) rx = 0;
    if(ry < 0) ry = 0;

    /* \bord65535 widens a glyph to a million pixels square, which would take
     * minutes per frame. Past the limits ScanConvert applies to shapes, the
     * word is not drawn. Units are eighths of a pixel. */
    const int64_t w = (int64_t)mWidth + 2 * (int64_t)rx, h = (int64_t)mHeight + 2 * (int64_t)ry;
    /* The widening below overlaps the whole outline once per row of the
     * border's half circle, so its cost is the product of the two: a glyph
     * stretched a million pixels tall with \fscy and a border scaled with it
     * passes the size limits and would still take hours. Real subtitles stay
     * far below this bound: a full-frame 1080p glyph with \bord100, or a
     * 3000-pixel one with \bord300, does about 1/45 of it. */
    const int64_t work = (2 * (int64_t)ry + 1) * (int64_t)mOutline.size();
    if(w > kMaxShapeExtent || h > kMaxShapeExtent || w * h > kMaxShapeArea
       || work > ((int64_t)1 << 30))
    {
        mWidth = mHeight = 0;
        mOutline.clear();
        mWideOutline.clear();
        mWideBorder = 0;
        return true;
    }

    mWideBorder = std::max(rx, ry);

    if(ry > 0)
    {
        // Do a half circle.
        // _OverlapRegion mirrors this so both halves are done.
        for(ptrdiff_t y = -ry; y <= ry; ++y)
        {
            int x = (int)(0.5 + sqrt(float(ry * ry - y * y)) * float(rx) / float(ry));

            _OverlapRegion(mWideOutline, mOutline, x, y);
        }
    }
    else if(ry == 0 && rx > 0)
    {
        // There are artifacts if we don't make at least two overlaps of the line, even at same Y coord
        _OverlapRegion(mWideOutline, mOutline, rx, 0);
        _OverlapRegion(mWideOutline, mOutline, rx, 0);
    }

    return true;
}

void Rasterizer::DeleteOutlines()
{
    mWideOutline.clear();
    mOutline.clear();
}

bool Rasterizer::Rasterize(int xsub, int ysub, int fBlur, double fGaussianBlur)
{
    _TrashOverlay();

    if(!mWidth || !mHeight)
    {
        mOverlayWidth = mOverlayHeight = 0;
        return true;
    }

    /* Upper bounds, as libass has, on the two blurs: their cost grows with
     * the blur, and beyond these they are invisible anyway. */
    if(fBlur < 0) fBlur = 0;
    if(fBlur > 255) fBlur = 255;
    if(!(fGaussianBlur > 0)) fGaussianBlur = 0;        // also NaN
    if(fGaussianBlur > 256) fGaussianBlur = 256;

    xsub &= 7;
    ysub &= 7;

    int width = mWidth + xsub;
    int height = mHeight;// + ysub

    mOffsetX = mPathOffsetX - xsub;
    mOffsetY = mPathOffsetY - ysub;

    mWideBorder = (mWideBorder + 7)&~7;

    if(!mWideOutline.empty() || fBlur || fGaussianBlur > 0)
    {
        int bluradjust = 0;
        if(fGaussianBlur > 0)
            bluradjust += (int)(fGaussianBlur * 3 * 8 + 0.5) | 1;
        if(fBlur)
            bluradjust += 8;

        // Expand the buffer a bit when we're blurring, since that can also widen the borders a bit
        bluradjust = (bluradjust + 7)&~7;

        width += 2 * mWideBorder + bluradjust * 2;
        height += 2 * mWideBorder + bluradjust * 2;

        xsub += mWideBorder + bluradjust;
        ysub += mWideBorder + bluradjust;

        mOffsetX -= mWideBorder + bluradjust;
        mOffsetY -= mWideBorder + bluradjust;
    }

    mOverlayWidth = ((width + 7) >> 3) + 1;
    // fixed image height
    mOverlayHeight = ((height + 14) >> 3) + 1;

    if((int64_t)mOverlayWidth * mOverlayHeight > ((int64_t)1 << 28))
    {
        mOverlayWidth = mOverlayHeight = 0;     // see ScanConvert's limit
        return true;
    }

    mpOverlayBuffer = DNew BYTE[2 * mOverlayWidth * mOverlayHeight];
    memset(mpOverlayBuffer, 0, 2 * mOverlayWidth * mOverlayHeight);

    // Are we doing a border?

    tSpanBuffer* pOutline[2] = {&mOutline, &mWideOutline};

    for(ptrdiff_t i = countof(pOutline) - 1; i >= 0; i--)
    {
        tSpanBuffer::iterator it = pOutline[i]->begin();
        tSpanBuffer::iterator itEnd = pOutline[i]->end();

        for(; it != itEnd; ++it)
        {
            size_t y = (((*it).first >> 32) - 0x40000000 + ysub);
            size_t x1 = (((*it).first & 0xffffffff) - 0x40000000 + xsub);
            size_t x2 = (((*it).second & 0xffffffff) - 0x40000000 + xsub);

            if(x2 > x1)
            {
                size_t first = x1 >> 3;
                size_t last = (x2 - 1) >> 3;
                BYTE* dst = mpOverlayBuffer + 2 * (mOverlayWidth * (y >> 3) + first) + i;

                if(first == last)
                    *dst += x2 - x1;
                else
                {
                    *dst += ((first + 1) << 3) - x1;
                    dst += 2;

                    while(++first < last)
                    {
                        *dst += 0x08;
                        dst += 2;
                    }

                    *dst += x2 - (last << 3);
                }
            }
        }
    }

    // Do some gaussian blur magic
    if(fGaussianBlur > 0)
    {
        GaussianKernel filter(fGaussianBlur);
        if(mOverlayWidth >= filter.width && mOverlayHeight >= filter.width)
        {
            size_t pitch = mOverlayWidth * 2;

            BYTE *tmp = DNew BYTE[pitch*mOverlayHeight];
            if(!tmp) return(false);

            int border = !mWideOutline.empty() ? 1 : 0;

            BYTE *src = mpOverlayBuffer + border;

            SeparableFilterX<2>(src, tmp, mOverlayWidth, mOverlayHeight, pitch, filter.kernel, filter.width, filter.divisor);
            SeparableFilterY<2>(tmp, src, mOverlayWidth, mOverlayHeight, pitch, filter.kernel, filter.width, filter.divisor);

            delete[] tmp;
        }
    }

    // If we're blurring, do a 3x3 box blur
    // Can't do it on subpictures smaller than 3x3 pixels
    for(int pass = 0; pass < fBlur; pass++)
    {
        if(mOverlayWidth >= 3 && mOverlayHeight >= 3)
        {
            int pitch = mOverlayWidth * 2;

            BYTE* tmp = DNew BYTE[pitch*mOverlayHeight];
            if(!tmp) return(false);

            memcpy(tmp, mpOverlayBuffer, pitch * mOverlayHeight);

            int border = !mWideOutline.empty() ? 1 : 0;

            // This could be done in a separated way and win some speed
            for(ptrdiff_t j = 1; j < mOverlayHeight - 1; j++)
            {
                BYTE* src = tmp + pitch * j + 2 + border;
                BYTE* dst = mpOverlayBuffer + pitch * j + 2 + border;

                for(ptrdiff_t i = 1; i < mOverlayWidth - 1; i++, src += 2, dst += 2)
                {
                    *dst = (src[-2-pitch] + (src[-pitch] << 1) + src[+2-pitch]
                            + (src[-2] << 1) + (src[0] << 2) + (src[+2] << 1)
                            + src[-2+pitch] + (src[+pitch] << 1) + src[+2+pitch]) >> 4;
                }
            }

            delete [] tmp;
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////
//
// Blend kernels.
//
// Upstream had these twice: SSE2 versions, which every x86 CPU of the last
// twenty years ran, and plain C versions for CPUs without SSE2, each with its
// own copy of every Draw_* routine. The C copies had drifted -- some switched
// karaoke colours where the SSE2 ones did not and vice versa -- so a build
// without SSE2 (AArch64) rendered \kf, gradients and clips
// differently. There is now one set of routines, the ones upstream ran, on
// the C kernels below. Those compute exactly what the SSE2 ones did, for
// every input: each channel is (dst * (256 - a) + src * (a + 1)) >> 8, which
// fits in 16 bits either way, and the destination alpha is scaled alone.

static inline void pixmix(DWORD *dst, DWORD color, DWORD alpha)
{
    // coverage alpha is 6-bit (0..64); add a rounding term so the >> 6 rounds
    // to nearest instead of truncating, which otherwise darkens mid tones.
    DWORD a = (((alpha) * (color >> 24) + (1 << 5)) >> 6) & 0xff;
    DWORD ia = 256 - a;
    a += 1;

    *dst = ((((*dst & 0x00ff00ff) * ia + (color & 0x00ff00ff) * a) & 0xff00ff00) >> 8)
           | ((((*dst & 0x0000ff00) * ia + (color & 0x0000ff00) * a) & 0x00ff0000) >> 8)
           | ((((*dst >> 8) & 0x00ff0000) * ia) & 0xff000000);
}

static inline void pixmix2(DWORD *dst, DWORD color, DWORD shapealpha, DWORD clipalpha)
{
    // shape*clip coverage is 12-bit (0..64*64); round to nearest on >> 12.
    DWORD a = (((shapealpha) * (clipalpha) * (color >> 24) + (1 << 11)) >> 12) & 0xff;
    DWORD ia = 256 - a;
    a += 1;

    *dst = ((((*dst & 0x00ff00ff) * ia + (color & 0x00ff00ff) * a) & 0xff00ff00) >> 8)
           | ((((*dst & 0x0000ff00) * ia + (color & 0x0000ff00) * a) & 0x00ff0000) >> 8)
           | ((((*dst >> 8) & 0x00ff0000) * ia) & 0xff000000);
}

// a - b clamped to 0, in each 16-bit half, as PSUBUSW did.
static inline DWORD safe_subtract(DWORD a, DWORD b)
{
    const DWORD lo = (a & 0xffff) > (b & 0xffff) ? (a & 0xffff) - (b & 0xffff) : 0;
    const DWORD hi = (a >> 16) > (b >> 16) ? (a >> 16) - (b >> 16) : 0;
    return (hi << 16) | lo;
}

/* The column at which a karaoke colour switch happens, clamped to the row.
 * Upstream clamped it only from above; a switch point far to the left (a
 * \bord of 2^31, say) made the second-colour loop start at a negative
 * column and read and write outside the overlay and the frame. */
static inline int SwitchColumn(const RasterizerNfo& rnfo)
{
    const int64_t gran = (int64_t)(int)rnfo.sw[3] + 1 - rnfo.xo;
    return (int)std::max<int64_t>(0, std::min<int64_t>(gran, rnfo.w));
}

void Rasterizer::Draw_noAlpha_spFF_Body(RasterizerNfo& rnfo)
{
    int h = rnfo.h;
    int color = rnfo.color;

    const DWORD* sw = rnfo.sw;
    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;
    // The <<6 is due to pixmix expecting the alpha parameter to be
    // the multiplication of two 6-bit unsigned numbers but we
    // only have one here. (No alpha mask.)
    while(h--)
    {
        for(int wt = 0; wt < rnfo.w; ++wt)
            pixmix(&dst[wt], color, s[wt*2]);
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}//Draw_noAlpha_spFF_Body(w,h,color,spd.pitch,dst,s);

void Rasterizer::Draw_noAlpha_spFF_noBody(RasterizerNfo& rnfo)
{
    int h = rnfo.h;
    int color = rnfo.color;

    const DWORD* sw = rnfo.sw;
    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;
    // src contains two different bitmaps, interlaced per pixel.
    // The first stored is the fill, the second is the widened
    // fill region created by CreateWidenedRegion().
    // Since we're drawing only the border, we must otain that
    // by subtracting the fill from the widened region. The
    // subtraction must be saturating since the widened region
    // pixel value can be smaller than the fill value.
    // This happens when blur edges is used.
    while(h--)
    {
        for(int wt = 0; wt < rnfo.w; ++wt)
            pixmix(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]));
        src += 2 * rnfo.overlayp;

        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}//Draw_noAlpha_spFF_noBody(w,h,color,spd.pitch,dst,src);

void Rasterizer::Draw_noAlpha_sp_Body(RasterizerNfo& rnfo)
{
    int h = rnfo.h;

    int color = rnfo.color;

    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;
    // xo is the offset (usually negative) we have moved into the image
    // So if we have passed the switchpoint (?) switch to another colour
    // (So switchpts stores both colours *and* coordinates?)
    int gran = SwitchColumn(rnfo);
    int color2 = rnfo.sw[2];
    while(h--)
    {
        for(int wt = 0; wt < gran; ++wt)
            pixmix(&dst[wt], color, s[wt*2]);
        for(int wt = gran; wt < rnfo.w; ++wt)
            pixmix(&dst[wt], color2, s[wt*2]);
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_noAlpha_sp_noBody(RasterizerNfo& rnfo)
{
    int h = rnfo.h;
    int color = rnfo.color;


    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;
    int gran = SwitchColumn(rnfo);
    int color2 = rnfo.sw[2];
    while(h--)
    {
        for(int wt = 0; wt < gran; ++wt)
            pixmix(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]));
        for(int wt = gran; wt < rnfo.w; ++wt)
            pixmix(&dst[wt], color2, safe_subtract(src[wt*2+1], src[wt*2]));
        src += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Alpha_spFF_Body(RasterizerNfo& rnfo)
{
#ifdef _VSMOD // patch m006. moveable vector clip
    MOD_MOVEVC mod_vc = rnfo.mod_vc;
#else
    BYTE* am = rnfo.am;
#endif
    int h = rnfo.h;
    int color = rnfo.color;

    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;
    // Both s and am contain 6-bit bitmaps of two different
    // alpha masks; s is the subtitle shape and am is the
    // clipping mask.
    // Multiplying them together yields a 12-bit number.
    // I think some imprecision is introduced here??
    while(h--)
    {
        for(int wt = 0; wt < rnfo.w; ++wt)
#ifdef _VSMOD // patch m006. moveable vector clip
            pixmix2(&dst[wt], color, s[wt*2], mod_vc.GetAlphaValue(wt, h));
#else
            pixmix2(&dst[wt], color, s[wt*2], am[wt]);
        am += rnfo.spdw;
#endif
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Alpha_spFF_noBody(RasterizerNfo& rnfo)
{
#ifdef _VSMOD // patch m006. moveable vector clip
    MOD_MOVEVC mod_vc = rnfo.mod_vc;
#else
    BYTE* am = rnfo.am;
#endif
    int h = rnfo.h;
    int color = rnfo.color;

    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;

    while(h--)
    {
        for(int wt = 0; wt < rnfo.w; ++wt)
#ifdef _VSMOD // patch m006. moveable vector clip
            pixmix2(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]), mod_vc.GetAlphaValue(wt, h));
#else
            pixmix2(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]), am[wt]);
        am += rnfo.spdw;
#endif
        src += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Alpha_sp_Body(RasterizerNfo& rnfo)
{
#ifdef _VSMOD // patch m006. moveable vector clip
    MOD_MOVEVC mod_vc = rnfo.mod_vc;
#else
    BYTE* am = rnfo.am;
#endif
    int h = rnfo.h;
    int color = rnfo.color;


    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;

    int gran = SwitchColumn(rnfo);
    int color2 = rnfo.sw[2];
    while(h--)
    {
#ifdef _VSMOD // patch m006. moveable vector clip
        for(int wt = 0; wt < gran; ++wt)
            pixmix2(&dst[wt], color, s[wt*2], mod_vc.GetAlphaValue(wt, h));
        for(int wt = gran; wt < rnfo.w; ++wt)
            pixmix2(&dst[wt], color2, s[wt*2], mod_vc.GetAlphaValue(wt, h));
#else
        for(int wt = 0; wt < gran; ++wt)
            pixmix2(&dst[wt], color, s[wt*2], am[wt]);
        for(int wt = gran; wt < rnfo.w; ++wt)
            pixmix2(&dst[wt], color2, s[wt*2], am[wt]);
        am += rnfo.spdw;
#endif
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Alpha_sp_noBody(RasterizerNfo& rnfo)
{
#ifdef _VSMOD // patch m006. moveable vector clip
    MOD_MOVEVC mod_vc = rnfo.mod_vc;
#else
    BYTE* am = rnfo.am;
#endif
    int h = rnfo.h;

    DWORD color = rnfo.color;

    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;

    int gran = SwitchColumn(rnfo);
    int color2 = rnfo.sw[2];
    while(h--)
    {
#ifdef _VSMOD // patch m006. moveable vector clip
        for(int wt = 0; wt < gran; ++wt)
            pixmix2(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]), mod_vc.GetAlphaValue(wt, h));
        for(int wt = gran; wt < rnfo.w; ++wt)
            pixmix2(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]), mod_vc.GetAlphaValue(wt, h));
#else
        for(int wt = 0; wt < gran; ++wt)
            pixmix2(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]), am[wt]);
        for(int wt = gran; wt < rnfo.w; ++wt)
            pixmix2(&dst[wt], color, safe_subtract(src[wt*2+1], src[wt*2]), am[wt]);
        am += rnfo.spdw;
#endif
        src += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

#ifdef _VSMOD // patch m004. gradient colors
void Rasterizer::Draw_Grad_noAlpha_spFF_Body(RasterizerNfo& rnfo)
{
    double hfull = (double)rnfo.h;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;
    int typ = rnfo.typ;

    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;

    int h = rnfo.h;
    int w = rnfo.w;
    while(h--)
    {
        for(int wt = 0; wt < w; ++wt)
            pixmix(&dst[wt], mod_grad.getmixcolor(wt, h, typ), s[wt*2]);
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Grad_noAlpha_spFF_noBody(RasterizerNfo& rnfo)
{
    double hfull = (double)rnfo.h;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;
    int typ = rnfo.typ;

    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;

    int h = rnfo.h;
    int w = rnfo.w;
    while(h--)
    {
        for(int wt = 0; wt < w; ++wt)
            pixmix(&dst[wt], mod_grad.getmixcolor(wt, h, typ), safe_subtract(src[wt*2+1], src[wt*2]));
        src += 2 * rnfo.overlayp;

        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Grad_noAlpha_sp_Body(RasterizerNfo& rnfo)
{
    double hfull = (double)rnfo.h;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;

    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;

    int h = rnfo.h;
    int w = rnfo.w;
    int gran = SwitchColumn(rnfo);
    while(h--)
    {
        for(int wt = 0; wt < gran; ++wt)
            pixmix(&dst[wt], mod_grad.getmixcolor(wt, h, 0), s[wt*2]);
        for(int wt = gran; wt < w; ++wt)
            pixmix(&dst[wt], mod_grad.getmixcolor(wt, h, 1), s[wt*2]);
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Grad_noAlpha_sp_noBody(RasterizerNfo& rnfo)
{
    double hfull = (double)rnfo.h;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;

    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;

    int h = rnfo.h;
    int w = rnfo.w;
    int gran = SwitchColumn(rnfo);
    while(h--)
    {
        for(int wt = 0; wt < gran; ++wt)
            pixmix(&dst[wt], mod_grad.getmixcolor(wt, h, 0), safe_subtract(src[wt*2+1], src[wt*2]));
        for(int wt = gran; wt < w; ++wt)
            pixmix(&dst[wt], mod_grad.getmixcolor(wt, h, 1), safe_subtract(src[wt*2+1], src[wt*2]));
        src += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Grad_Alpha_spFF_Body(RasterizerNfo& rnfo)
//(int w, int h, int overlayp, int typ, MOD_GRADIENT& mod_grad, int pitch, DWORD* dst, const BYTE* s, MOD_MOVEVC& mod_vc)
{
    double hfull = (double)rnfo.h;
    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;
    MOD_MOVEVC mod_vc = rnfo.mod_vc;

    int typ = rnfo.typ;
    int h = rnfo.h;
    int w = rnfo.w;
    while(h--)
    {
        for(int wt = 0; wt < w; ++wt)
            pixmix2(&dst[wt], mod_grad.getmixcolor(wt, h, typ), s[wt*2], mod_vc.GetAlphaValue(wt, h));
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Grad_Alpha_spFF_noBody(RasterizerNfo& rnfo)
//(int w, int h, int overlayp, int typ, MOD_GRADIENT& mod_grad, int pitch, DWORD* dst, const BYTE* src, MOD_MOVEVC& mod_vc)
{
    double hfull = (double)rnfo.h;
    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;
    MOD_MOVEVC mod_vc = rnfo.mod_vc;

    int typ = rnfo.typ;
    int h = rnfo.h;
    int w = rnfo.w;
    while(h--)
    {
        for(int wt = 0; wt < w; ++wt)
            pixmix2(&dst[wt], mod_grad.getmixcolor(wt, h, typ), safe_subtract(src[wt*2+1], src[wt*2]), mod_vc.GetAlphaValue(wt, h));
        src += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Grad_Alpha_sp_Body(RasterizerNfo& rnfo)
{
    double hfull = (double)rnfo.h;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;
    MOD_MOVEVC mod_vc = rnfo.mod_vc;

    BYTE* s = rnfo.s;
    DWORD* dst = rnfo.dst;

    int h = rnfo.h;
    int w = rnfo.w;
    int gran = SwitchColumn(rnfo);
    while(h--)
    {
        for(int wt = 0; wt < gran; ++wt)
            pixmix2(&dst[wt], mod_grad.getmixcolor(wt, h, 0), s[wt*2], mod_vc.GetAlphaValue(wt, h));
        for(int wt = gran; wt < w; ++wt)
            pixmix2(&dst[wt], mod_grad.getmixcolor(wt, h, 1), s[wt*2], mod_vc.GetAlphaValue(wt, h));
        s += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}

void Rasterizer::Draw_Grad_Alpha_sp_noBody(RasterizerNfo& rnfo)
{
    double hfull = (double)rnfo.h;
    MOD_GRADIENT mod_grad = rnfo.mod_grad;
    MOD_MOVEVC mod_vc = rnfo.mod_vc;

    BYTE* src = rnfo.src;
    DWORD* dst = rnfo.dst;

    int h = rnfo.h;
    int w = rnfo.w;
    int gran = SwitchColumn(rnfo);
    while(h--)
    {
        for(int wt = 0; wt < gran; ++wt)
            pixmix2(&dst[wt], mod_grad.getmixcolor(wt, h, 0), safe_subtract(src[wt*2+1], src[wt*2]), mod_vc.GetAlphaValue(wt, h));
        for(int wt = gran; wt < w; ++wt)
            pixmix2(&dst[wt], mod_grad.getmixcolor(wt, h, 1), safe_subtract(src[wt*2+1], src[wt*2]), mod_vc.GetAlphaValue(wt, h));
        src += 2 * rnfo.overlayp;
        dst = (DWORD*)((char *)dst + rnfo.pitch);
    }
}
#endif
// Render a subpicture onto a surface.
// spd is the surface to render on.
// clipRect is a rectangular clip region to render inside.
// pAlphaMask is an alpha clipping mask.
// xsub and ysub ???
// switchpts seems to be an array of fill colours interlaced with coordinates.
//    switchpts[i*2] contains a colour and switchpts[i*2+1] contains the coordinate to use that colour from
// fBody tells whether to render the body of the subs.
// fBorder tells whether to render the border of the subs.
#ifdef _VSMOD // patch m004. gradient colors
CRect Rasterizer::Draw(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, int xsub, int ysub,
                       const DWORD* switchpts, bool fBody, bool fBorder, int typ, MOD_GRADIENT& mod_grad, MOD_MOVEVC& mod_vc)
#else
CRect Rasterizer::Draw(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, int xsub, int ysub,
                       const DWORD* switchpts, bool fBody, bool fBorder)
#endif
{
    CRect bbox(0, 0, 0, 0);

    if(!switchpts || !fBody && !fBorder) return(bbox);

    // Limit drawn area to intersection of rendering surface and rectangular clip area
    CRect r(0, 0, spd.w, spd.h);
    r &= clipRect;

    // Remember that all subtitle coordinates are specified in 1/8 pixels
    // (x+4)>>3 rounds to nearest whole pixel.
    // ??? What is xsub, ysub, mOffsetX and mOffsetY ?
    //
    // Clipped in 64 bits: with extreme positions or borders (\bord2147483647)
    // the 32-bit sums wrapped, a glyph far off screen passed the tests below
    // and was blitted outside the frame. Once clipped, every value lies
    // within the frame and the overlay and fits an int again.
    int64_t x64 = ((int64_t)xsub + mOffsetX + 4) >> 3;
    int64_t y64 = ((int64_t)ysub + mOffsetY + 4) >> 3;
    int64_t w64 = mOverlayWidth;
    int64_t h64 = mOverlayHeight;
    int64_t xo64 = 0, yo64 = 0;

    const int yBeforeClip = (int)std::max<int64_t>(INT_MIN, std::min<int64_t>(INT_MAX, y64));

    if(x64 < r.left)
    {
        xo64 = r.left - x64;
        w64 -= xo64;
        x64 = r.left;
    }
    if(y64 < r.top)
    {
        yo64 = r.top - y64;
        h64 -= yo64;
        y64 = r.top;
    }
    if(x64 + w64 > r.right) w64 = r.right - x64;
    if(y64 + h64 > r.bottom) h64 = r.bottom - y64;

    // Check if there's actually anything to render
    if(w64 <= 0 || h64 <= 0 || !mpOverlayBuffer) return(bbox);

    int x = (int)x64, y = (int)y64, w = (int)w64, h = (int)h64;
    int xo = (int)xo64, yo = (int)yo64;

    bbox.SetRect(x, y, x + w, y + h);
    bbox &= CRect(0, 0, spd.w, spd.h);

#ifdef _VSMOD // patch m006. moveable vector clip
    mod_vc.hfull = h;
    mod_vc.curpos = CPoint(x, y);
    mod_vc.alphamask = pAlphaMask + spd.w * y + x;
#endif

    // fill rasterize info
    RasterizerNfo rnfo;
    // Grab the first colour
    rnfo.color = switchpts[0];
    // How would this differ from src?
    rnfo.dst = (DWORD*)((char *)spd.bits + (spd.pitch * y)) + x;
    rnfo.sw = switchpts;

    rnfo.w = w;
    rnfo.h = h;
    rnfo.xo = xo;
    rnfo.yo = yo;
    rnfo.overlayp = mOverlayWidth;
    rnfo.pitch = spd.pitch;
    rnfo.spdw = spd.w;
    // The alpha bitmap of the subtitles?
    rnfo.src = mpOverlayBuffer + 2 * (mOverlayWidth * yo + xo);
    // s points to what the "body" to use is
    // If we're rendering body fill and border, src+1 points to the array of
    // widened regions which contain both border and fill in one.
    rnfo.s = fBorder ? (rnfo.src + 1) : rnfo.src;
#ifdef _VSMOD // patch m006. moveable vector clip
    rnfo.mod_vc = mod_vc;
    rnfo.mod_grad = mod_grad;
    rnfo.mod_grad.width = mOverlayWidth;
    rnfo.mod_grad.height = mOverlayHeight;
    rnfo.mod_grad.xoffset = xo;
    rnfo.mod_grad.yoffset = yo;
    rnfo.typ = typ;
    rnfo.mod_grad.clipDiff = clipRect.bottom < yBeforeClip + mOverlayHeight ? yBeforeClip + mOverlayHeight - clipRect.bottom : 0;
#else
    // The complex "vector clip mask" I think.
    rnfo.am = pAlphaMask + spd.w * y + x;
#endif
    // Every remaining line in the bitmap to be rendered...
    // Basic case of no complex clipping mask
#ifdef _VSMOD // patch m004. gradient colors
    if (
        ((typ == 0) && ((mod_grad.mode[0] == 0) && (mod_grad.mode[1] == 0)))
        || ((typ != 0) && (mod_grad.mode[typ] == 0))
        )
        // No gradient
    {
#endif
        if (!pAlphaMask)
        {
            // If the first colour switching coordinate is at "infinite" we're
            // never switching and can use some simpler code.
            // ??? Is this optimisation really worth the extra readability issues it adds?
            if (switchpts[1] == 0xFFFFFFFF)
            {
                // fBody is true if we're rendering a fill or a shadow.
                if (fBody)
                {
                    Draw_noAlpha_spFF_Body(rnfo);
                }
                // Not painting body, ie. painting border without fill in it
                else
                {
                    Draw_noAlpha_spFF_noBody(rnfo);
                }
            }
            // not (switchpts[1] == 0xFFFFFFFF)
            else
            {
                // switchpts plays an important rule here
                //const long *sw = switchpts;

                if (fBody)
                {
                    Draw_noAlpha_sp_Body(rnfo);
                }
                // Not body
                else
                {
                    Draw_noAlpha_sp_noBody(rnfo);
                }
            }
        }
        // Here we *do* have an alpha mask
        else
        {
            if (switchpts[1] == 0xFFFFFFFF)
            {
                if (fBody)
                {
                    Draw_Alpha_spFF_Body(rnfo);
                }
                else
                {
                    Draw_Alpha_spFF_noBody(rnfo);
                }
            }
            else
            {
                //const long *sw = switchpts;

                if (fBody)
                {
                    Draw_Alpha_sp_Body(rnfo);
                }
                else
                {
                    Draw_Alpha_sp_noBody(rnfo);
                }
            }
        }
#ifdef _VSMOD // patch m004. gradient colors
    }
    else
    {
        if(!pAlphaMask)
        {
            // If the first colour switching coordinate is at "infinite" we're
            // never switching and can use some simpler code.
            // ??? Is this optimisation really worth the extra readability issues it adds?
            if(switchpts[1] == 0xFFFFFFFF)
            {
                // fBody is true if we're rendering a fill or a shadow.
                if(fBody)
                {
                    Draw_Grad_noAlpha_spFF_Body(rnfo);
                }
                // Not painting body, ie. painting border without fill in it
                else
                {
                    Draw_Grad_noAlpha_spFF_noBody(rnfo);
                }
            }
            // not (switchpts[1] == 0xFFFFFFFF)
            else
            {
                // switchpts plays an important rule here
                //const long *sw = switchpts;

                if(fBody)
                {
                    Draw_Grad_noAlpha_sp_Body(rnfo);
                }
                // Not body
                else
                {
                    Draw_Grad_noAlpha_sp_noBody(rnfo);
                }
            }
        }
        // Here we *do* have an alpha mask
        else
        {
            if(switchpts[1] == 0xFFFFFFFF)
            {
                if(fBody)
                {
                    Draw_Grad_Alpha_spFF_Body(rnfo);
                }
                else
                {
                    Draw_Grad_Alpha_spFF_noBody(rnfo);
                }
            }
            else
            {
                //const long *sw = switchpts;

                if(fBody)
                {
                    Draw_Grad_Alpha_sp_Body(rnfo);
                }
                else
                {
                    Draw_Grad_Alpha_sp_noBody(rnfo);
                }
            }
        }
    }

// patch m010. png background
//if(typ==0) rnfo.mod_grad.b_images[1].freeImage();
//rnfo.mod_grad.b_images[typ].freeImage();
#endif

    return bbox;
}


void Rasterizer::FillSolidRect(SubPicDesc& spd, int x, int y, int nWidth, int nHeight, DWORD lColor)
{
    for(int wy = y; wy < y + nHeight; wy++)
    {
        DWORD* dst = (DWORD*)((BYTE*)spd.bits + spd.pitch * wy) + x;

        for(int wt = 0; wt < nWidth; ++wt)
            pixmix(&dst[wt], lColor, 0x40);	// 0x40 because >> 6 in pixmix (to preserve tranparency)
    }
}

#ifdef _VSMOD // patch m006. moveable vector clip
MOD_MOVEVC::MOD_MOVEVC()
{
    clear();
}

void MOD_MOVEVC::clear()
{
    enable = false;
    size = CSize(0, 0);
    pos = CPoint(0, 0);
//	canvas = CSize(0,0);
    spd = CSize(0, 0);
    curpos = CPoint(0, 0);
    hfull = 0;
    alphamask = NULL;
    isInverse = false;
}

BYTE MOD_MOVEVC::GetAlphaValue(int wx, int wy)
{
    BYTE alpham;
    if(!enable)
    {
//		return 0xFF;
        return alphamask[wx+((hfull-wy)*spd.cx)];
    }

    int xInCanvas = wx - pos.x + curpos.x;
    int yInCanvas = -pos.y + (hfull - wy) + curpos.y;

    //check if the point is in canvas, in case of crash
    if (xInCanvas < 0 || xInCanvas >= spd.cx)
        alpham = isInverse ? 0x40 : 0; // assume content outside canvas is visible when using iclip. mask is 6 bit
    else if (yInCanvas < 0 || yInCanvas >= spd.cy)
        alpham = isInverse ? 0x40 : 0;
    else
    {
        if ((wx - pos.x) < -curpos.x + 1) alpham = 0;
        else if ((wx - pos.x) > spd.cx - 1) alpham = 0; //canvas.cx
        else if ((hfull - wy - pos.y) < -curpos.y + 1) alpham = 0;
        else if ((hfull - wy - pos.y) > spd.cy - 1) alpham = 0;
        else alpham = alphamask[wx - pos.x - pos.y*spd.cx + ((hfull - wy)*spd.cx)];
    }

    return alpham;
}
#endif

RasterizerNfo::RasterizerNfo()
{
    /*
    w = 0;
    h = 0;
    spdw = 0;
    overlayp = 0;
    typ = 0;
    pitch = 0;
    color = 0;

    xo = 0;

    sw = NULL;
    s = NULL;
    src = NULL;
    dst = NULL;

    #ifdef _VSMOD
    mod_grad.clear();
    mod_vc.clear();
    #else
    am = NULL;
    #endif
    */
}