/*
 *  VSFilterMod cross-platform port -- reproducible transcendental functions.
 *  See det_math.h.
 *
 *  Ported from fdlibm 5.3 (k_sin.c, k_cos.c, e_rem_pio2.c, e_exp.c, e_log.c):
 *
 *    Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 *    Developed at SunPro, a Sun Microsystems, Inc. business.
 *    Permission to use, copy, modify, and distribute this
 *    software is freely granted, provided that this notice
 *    is preserved.
 *
 *  Changes: word access through memcpy; argument reduction uses fdlibm's
 *  medium-size Cody-Waite path for |x| < 2^19 * pi/2 and, beyond that (an
 *  angle of tens of millions of degrees), first reduces by an exact fmod of
 *  2*pi, which is less accurate than fdlibm's Payne-Hanek path but just as
 *  reproducible. pow is exp(y * log(x)).
 */

#include "det_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

/* Every operation below must round on its own: a fused multiply-add would
 * change the result on CPUs that have one. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace vsfm {

namespace {

inline int32_t High(double x)
{
    uint64_t b;
    std::memcpy(&b, &x, 8);
    return (int32_t)(b >> 32);
}

inline uint32_t Low(double x)
{
    uint64_t b;
    std::memcpy(&b, &x, 8);
    return (uint32_t)b;
}

inline double FromWords(int32_t hi, uint32_t lo)
{
    const uint64_t b = ((uint64_t)(uint32_t)hi << 32) | lo;
    double x;
    std::memcpy(&x, &b, 8);
    return x;
}

inline double SetHigh(double x, int32_t hi) { return FromWords(hi, Low(x)); }

// k_sin.c: sin(x + y) on [-pi/4, pi/4]; y is the tail of x, iy says whether
// it is meaningful.
double KernelSin(double x, double y, int iy)
{
    static const double
        half = 5.00000000000000000000e-01,
        S1 = -1.66666666666666324348e-01,
        S2 = 8.33333333332248946124e-03,
        S3 = -1.98412698298579493134e-04,
        S4 = 2.75573137070700676789e-06,
        S5 = -2.50507602534068634195e-08,
        S6 = 1.58969099521155010221e-10;

    const int32_t ix = High(x) & 0x7fffffff;
    if (ix < 0x3e400000) {                      // |x| < 2**-27
        if ((int)x == 0) return x;              // generate inexact
    }
    const double z = x * x;
    const double v = z * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (half * y - v * r) - y) - v * S1);
}

// k_cos.c: cos(x + y) on [-pi/4, pi/4].
double KernelCos(double x, double y)
{
    static const double
        one = 1.00000000000000000000e+00,
        C1 = 4.16666666666666019037e-02,
        C2 = -1.38888888888741095749e-03,
        C3 = 2.48015872894767294178e-05,
        C4 = -2.75573143513906633035e-07,
        C5 = 2.08757232129817482790e-09,
        C6 = -1.13596475577881948265e-11;

    const int32_t ix = High(x) & 0x7fffffff;
    if (ix < 0x3e400000) {                      // |x| < 2**-27
        if ((int)x == 0) return one;            // generate inexact
    }
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3FD33333)                        // |x| < 0.3
        return one - (0.5 * z - (z * r - x * y));

    double qx;
    if (ix > 0x3fe90000) qx = 0.28125;          // x > 0.78125
    else qx = FromWords(ix - 0x00200000, 0);    // x / 4
    const double hz = 0.5 * z - qx;
    const double a = one - qx;
    return a - (hz - (z * r - x * y));
}

// e_rem_pio2.c, medium-size path: x - n*pi/2 as y[0] + y[1]; returns n.
int RemPio2(double x, double* y)
{
    static const double
        half = 5.00000000000000000000e-01,
        invpio2 = 6.36619772367581382433e-01,
        pio2_1 = 1.57079632673412561417e+00,
        pio2_1t = 6.07710050650619224932e-11,
        pio2_2 = 6.07710050630396597660e-11,
        pio2_2t = 2.02226624879595063154e-21,
        pio2_3 = 2.02226624871116645580e-21,
        pio2_3t = 8.47842766036889956997e-32,
        twopi = 6.28318530717958623200e+00;     // 2*pi rounded to double

    int32_t hx = High(x);
    int32_t ix = hx & 0x7fffffff;

    if (ix > 0x413921fb) {
        /* Beyond 2^19 * pi/2: bring it into range first. fmod is exact on
         * every platform, so this is reproducible, if not as accurate as
         * fdlibm's multi-precision reduction. */
        x = std::fmod(x, twopi);
        hx = High(x);
        ix = hx & 0x7fffffff;
    }

    const double t = std::fabs(x);
    const int n = (int)(t * invpio2 + half);
    const double fn = (double)n;
    double r = t - fn * pio2_1;
    double w = fn * pio2_1t;                    // 1st round good to 85 bits

    const int32_t j = ix >> 20;
    y[0] = r - w;
    int32_t i = j - ((High(y[0]) >> 20) & 0x7ff);
    if (i > 16) {                               // 2nd iteration, good to 118 bits
        const double t2 = r;
        w = fn * pio2_2;
        r = t2 - w;
        w = fn * pio2_2t - ((t2 - r) - w);
        y[0] = r - w;
        i = j - ((High(y[0]) >> 20) & 0x7ff);
        if (i > 49) {                           // 3rd iteration, 151 bits
            const double t3 = r;
            w = fn * pio2_3;
            r = t3 - w;
            w = fn * pio2_3t - ((t3 - r) - w);
            y[0] = r - w;
        }
    }
    y[1] = (r - y[0]) - w;

    if (hx < 0) {
        y[0] = -y[0];
        y[1] = -y[1];
        return -n;
    }
    return n;
}

} // namespace

double DetSin(double x)
{
    const int32_t ix = High(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return KernelSin(x, 0.0, 0);     // |x| <= pi/4
    if (ix >= 0x7ff00000) return x - x;                     // inf or NaN

    double y[2];
    const int n = RemPio2(x, y);
    switch (n & 3) {
    case 0:  return KernelSin(y[0], y[1], 1);
    case 1:  return KernelCos(y[0], y[1]);
    case 2:  return -KernelSin(y[0], y[1], 1);
    default: return -KernelCos(y[0], y[1]);
    }
}

double DetCos(double x)
{
    const int32_t ix = High(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return KernelCos(x, 0.0);        // |x| <= pi/4
    if (ix >= 0x7ff00000) return x - x;                     // inf or NaN

    double y[2];
    const int n = RemPio2(x, y);
    switch (n & 3) {
    case 0:  return KernelCos(y[0], y[1]);
    case 1:  return -KernelSin(y[0], y[1], 1);
    case 2:  return -KernelCos(y[0], y[1]);
    default: return KernelSin(y[0], y[1], 1);
    }
}

// e_exp.c
double DetExp(double x)
{
    static const double
        one = 1.0,
        halF[2] = {0.5, -0.5},
        huge = 1.0e+300,
        twom1000 = 9.33263618503218878990e-302,     // 2**-1000
        o_threshold = 7.09782712893383973096e+02,
        u_threshold = -7.45133219101941108420e+02,
        ln2HI[2] = {6.93147180369123816490e-01, -6.93147180369123816490e-01},
        ln2LO[2] = {1.90821492927058770002e-10, -1.90821492927058770002e-10},
        invln2 = 1.44269504088896338700e+00,
        P1 = 1.66666666666666019037e-01,
        P2 = -2.77777777770155933842e-03,
        P3 = 6.61375632143793436117e-05,
        P4 = -1.65339022054652515390e-06,
        P5 = 4.13813679705723846039e-08;

    double hi = 0.0, lo = 0.0;
    int k = 0;

    int32_t hx = High(x);
    const int xsb = (hx >> 31) & 1;             // sign bit of x
    hx &= 0x7fffffff;                           // high word of |x|

    // filter out non-finite argument
    if (hx >= 0x40862E42) {                     // |x| >= 709.78...
        if (hx >= 0x7ff00000) {
            if (((hx & 0xfffff) | Low(x)) != 0) return x + x;   // NaN
            return (xsb == 0) ? x : 0.0;                         // exp(+-inf)
        }
        if (x > o_threshold) return huge * huge;                 // overflow
        if (x < u_threshold) return twom1000 * twom1000;         // underflow
    }

    // argument reduction
    if (hx > 0x3fd62e42) {                      // |x| > 0.5 ln2
        if (hx < 0x3FF0A2B2) {                  // and |x| < 1.5 ln2
            hi = x - ln2HI[xsb];
            lo = ln2LO[xsb];
            k = 1 - xsb - xsb;
        } else {
            k = (int)(invln2 * x + halF[xsb]);
            const double t = k;
            hi = x - t * ln2HI[0];              // t*ln2HI is exact here
            lo = t * ln2LO[0];
        }
        x = hi - lo;
    } else if (hx < 0x3e300000) {               // |x| < 2**-28
        if (huge + x > one) return one + x;     // trigger inexact
    } else {
        k = 0;
    }

    // x is now in primary range
    const double t = x * x;
    const double c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0) return one - ((x * c) / (c - 2.0) - x);
    const double y = one - ((lo - (x * c) / (2.0 - c)) - hi);
    if (k >= -1021) return SetHigh(y, High(y) + (k << 20));
    return SetHigh(y, High(y) + ((k + 1000) << 20)) * twom1000;
}

// e_log.c
double DetLog(double x)
{
    static const double
        ln2_hi = 6.93147180369123816490e-01,
        ln2_lo = 1.90821492927058770002e-10,
        two54 = 1.80143985094819840000e+16,
        Lg1 = 6.666666666666735130e-01,
        Lg2 = 3.999999999940941908e-01,
        Lg3 = 2.857142874366239149e-01,
        Lg4 = 2.222219843214978396e-01,
        Lg5 = 1.818357216161805012e-01,
        Lg6 = 1.531383769920937332e-01,
        Lg7 = 1.479819860511658591e-01;
    static const double zero = 0.0;

    int32_t hx = High(x);
    const uint32_t lx = Low(x);

    int k = 0;
    if (hx < 0x00100000) {                      // x < 2**-1022
        if (((hx & 0x7fffffff) | lx) == 0) return -two54 / zero;  // log(+-0) = -inf
        if (hx < 0) return (x - x) / zero;                          // log(-#) = NaN
        k -= 54;
        x *= two54;                             // subnormal, scale up x
        hx = High(x);
    }
    if (hx >= 0x7ff00000) return x + x;
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    int32_t i = (hx + 0x95f64) & 0x100000;
    x = SetHigh(x, hx | (i ^ 0x3ff00000));      // normalize x or x/2
    k += (i >> 20);
    const double f = x - 1.0;

    if ((0x000fffff & (2 + hx)) < 3) {          // |f| < 2**-20
        if (f == zero) {
            if (k == 0) return zero;
            const double dk = (double)k;
            return dk * ln2_hi + dk * ln2_lo;
        }
        const double R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        const double dk = (double)k;
        return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }

    const double s = f / (2.0 + f);
    const double dk = (double)k;
    const double z = s * s;
    i = hx - 0x6147a;
    const double w = z * z;
    const int32_t j = 0x6b851 - hx;
    const double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    const double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    const double R = t2 + t1;
    if (i > 0) {
        const double hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    }
    if (k == 0) return f - s * (f - R);
    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

double DetPow(double x, double y)
{
    if (y == 0.0 || x == 1.0) return 1.0;
    if (x != x || y != y) return x + y;                 // NaN
    if (x == 0.0) return y > 0.0 ? 0.0 : HUGE_VAL;
    if (x < 0.0) {
        /* Defined only for integral y; the sign follows its parity. */
        if (std::floor(y) != y) return (x - x) / (x - x);
        const double m = DetExp(y * DetLog(-x));
        return std::fmod(y, 2.0) != 0.0 ? -m : m;
    }
    return DetExp(y * DetLog(x));
}

} // namespace vsfm
