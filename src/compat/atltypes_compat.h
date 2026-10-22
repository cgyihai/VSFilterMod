/*
 *  VSFilterMod cross-platform port -- CPoint / CSize / CRect.
 *
 *  Layout-compatible with POINT / SIZE / RECT (they derive from them), so code
 *  that passes a CRect where a RECT* is expected keeps working.
 *
 *  MFC's CRect is "normalised on demand": IsRectEmpty() is true when left >=
 *  right or top >= bottom, and the bounding-box code in RTS relies on that
 *  rather than on rectangles always being well-ordered.
 */

#pragma once

#include "platform.h"
#include <algorithm>

class CSize;
class CRect;

class CPoint : public POINT
{
public:
    CPoint() { x = y = 0; }
    CPoint(LONG ix, LONG iy) { x = ix; y = iy; }
    CPoint(const POINT& p) { x = p.x; y = p.y; }

    void Offset(LONG dx, LONG dy) { x += dx; y += dy; }
    void Offset(const POINT& p) { x += p.x; y += p.y; }

    bool operator==(const POINT& p) const { return x == p.x && y == p.y; }
    bool operator!=(const POINT& p) const { return !(*this == p); }

    CPoint& operator+=(const POINT& p) { x += p.x; y += p.y; return *this; }
    CPoint& operator-=(const POINT& p) { x -= p.x; y -= p.y; return *this; }

    CPoint operator+(const POINT& p) const { return CPoint(x + p.x, y + p.y); }
    CPoint operator-(const POINT& p) const { return CPoint(x - p.x, y - p.y); }
    CPoint operator-() const { return CPoint(-x, -y); }

    inline CPoint operator+(const SIZE& s) const;
    inline CPoint operator-(const SIZE& s) const;
    inline CPoint& operator+=(const SIZE& s);
    inline CPoint& operator-=(const SIZE& s);
};

class CSize : public SIZE
{
public:
    CSize() { cx = cy = 0; }
    CSize(LONG icx, LONG icy) { cx = icx; cy = icy; }
    CSize(const SIZE& s) { cx = s.cx; cy = s.cy; }

    bool operator==(const SIZE& s) const { return cx == s.cx && cy == s.cy; }
    bool operator!=(const SIZE& s) const { return !(*this == s); }

    CSize& operator+=(const SIZE& s) { cx += s.cx; cy += s.cy; return *this; }
    CSize& operator-=(const SIZE& s) { cx -= s.cx; cy -= s.cy; return *this; }

    CSize operator+(const SIZE& s) const { return CSize(cx + s.cx, cy + s.cy); }
    CSize operator-(const SIZE& s) const { return CSize(cx - s.cx, cy - s.cy); }
    CSize operator-() const { return CSize(-cx, -cy); }
};

inline CPoint CPoint::operator+(const SIZE& s) const { return CPoint(x + s.cx, y + s.cy); }
inline CPoint CPoint::operator-(const SIZE& s) const { return CPoint(x - s.cx, y - s.cy); }
inline CPoint& CPoint::operator+=(const SIZE& s) { x += s.cx; y += s.cy; return *this; }
inline CPoint& CPoint::operator-=(const SIZE& s) { x -= s.cx; y -= s.cy; return *this; }

class CRect : public RECT
{
public:
    CRect() { left = top = right = bottom = 0; }
    CRect(LONG l, LONG t, LONG r, LONG b) { left = l; top = t; right = r; bottom = b; }
    CRect(const RECT& rc) { left = rc.left; top = rc.top; right = rc.right; bottom = rc.bottom; }
    CRect(const POINT& tl, const POINT& br) { left = tl.x; top = tl.y; right = br.x; bottom = br.y; }
    CRect(const POINT& tl, const SIZE& sz) { left = tl.x; top = tl.y; right = tl.x + sz.cx; bottom = tl.y + sz.cy; }

    LONG Width() const { return right - left; }
    LONG Height() const { return bottom - top; }
    CSize Size() const { return CSize(Width(), Height()); }

    CPoint& TopLeft() { return *((CPoint*)this); }
    const CPoint& TopLeft() const { return *((const CPoint*)this); }
    CPoint& BottomRight() { return *((CPoint*)this + 1); }
    const CPoint& BottomRight() const { return *((const CPoint*)this + 1); }

    CPoint CenterPoint() const { return CPoint((left + right) / 2, (top + bottom) / 2); }

    bool IsRectEmpty() const { return left >= right || top >= bottom; }
    bool IsRectNull() const { return left == 0 && top == 0 && right == 0 && bottom == 0; }

    void SetRect(LONG l, LONG t, LONG r, LONG b) { left = l; top = t; right = r; bottom = b; }
    void SetRect(const POINT& tl, const POINT& br) { SetRect(tl.x, tl.y, br.x, br.y); }
    void SetRectEmpty() { left = top = right = bottom = 0; }

    void NormalizeRect()
    {
        if (left > right) std::swap(left, right);
        if (top > bottom) std::swap(top, bottom);
    }

    void OffsetRect(LONG dx, LONG dy) { left += dx; right += dx; top += dy; bottom += dy; }
    void OffsetRect(const POINT& p) { OffsetRect(p.x, p.y); }
    void OffsetRect(const SIZE& s) { OffsetRect(s.cx, s.cy); }

    void InflateRect(LONG dx, LONG dy) { left -= dx; right += dx; top -= dy; bottom += dy; }
    void InflateRect(LONG l, LONG t, LONG r, LONG b) { left -= l; top -= t; right += r; bottom += b; }
    void DeflateRect(LONG dx, LONG dy) { InflateRect(-dx, -dy); }
    void DeflateRect(LONG l, LONG t, LONG r, LONG b) { InflateRect(-l, -t, -r, -b); }

    bool PtInRect(const POINT& p) const
    {
        return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
    }

    bool IntersectRect(const RECT& a, const RECT& b)
    {
        left   = std::max(a.left, b.left);
        top    = std::max(a.top, b.top);
        right  = std::min(a.right, b.right);
        bottom = std::min(a.bottom, b.bottom);
        if (IsRectEmpty()) { SetRectEmpty(); return false; }
        return true;
    }
    bool UnionRect(const RECT& a, const RECT& b)
    {
        CRect ra(a), rb(b);
        if (ra.IsRectEmpty()) { *this = rb; return !IsRectEmpty(); }
        if (rb.IsRectEmpty()) { *this = ra; return !IsRectEmpty(); }
        left   = std::min(a.left, b.left);
        top    = std::min(a.top, b.top);
        right  = std::max(a.right, b.right);
        bottom = std::max(a.bottom, b.bottom);
        return !IsRectEmpty();
    }

    bool operator==(const RECT& rc) const
    {
        return left == rc.left && top == rc.top && right == rc.right && bottom == rc.bottom;
    }
    bool operator!=(const RECT& rc) const { return !(*this == rc); }

    CRect& operator&=(const RECT& rc) { IntersectRect(*this, rc); return *this; }
    CRect& operator|=(const RECT& rc) { UnionRect(*this, rc); return *this; }

    CRect operator&(const RECT& rc) const { CRect r; r.IntersectRect(*this, rc); return r; }
    CRect operator|(const RECT& rc) const { CRect r; r.UnionRect(*this, rc); return r; }

    CRect& operator+=(const POINT& p) { OffsetRect(p); return *this; }
    CRect& operator-=(const POINT& p) { OffsetRect(-p.x, -p.y); return *this; }
    CRect& operator+=(const SIZE& s) { OffsetRect(s); return *this; }
    CRect& operator-=(const SIZE& s) { OffsetRect(-s.cx, -s.cy); return *this; }

    CRect operator+(const POINT& p) const { CRect r(*this); r.OffsetRect(p); return r; }
    CRect operator-(const POINT& p) const { CRect r(*this); r.OffsetRect(-p.x, -p.y); return r; }

    /* MFC overloads + and - with a RECT to mean inflate and deflate by that
     * rectangle's four members -- not offset. RTS relies on it to add and
     * remove line borders, so the asymmetry with the POINT overloads above is
     * intentional. */
    CRect operator+(const RECT& rc) const
    {
        return CRect(left - rc.left, top - rc.top, right + rc.right, bottom + rc.bottom);
    }
    CRect operator-(const RECT& rc) const
    {
        return CRect(left + rc.left, top + rc.top, right - rc.right, bottom - rc.bottom);
    }
    CRect& operator+=(const RECT& rc) { *this = *this + rc; return *this; }
    CRect& operator-=(const RECT& rc) { *this = *this - rc; return *this; }
};
