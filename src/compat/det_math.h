/*
 *  VSFilterMod cross-platform port -- transcendental functions that return
 *  the same bits on every platform.
 *
 *  sin, cos, exp, log and pow are not required to be correctly rounded, and
 *  the MSVC, glibc and Apple libraries do differ in the last bit for some
 *  inputs. Where such a result is truncated to an integer pixel coordinate
 *  -- (int)(cos(a) * r) -- one bit can move a glyph by a pixel. These are
 *  ports of fdlibm's implementations, which use only additions,
 *  multiplications and divisions (correctly rounded everywhere) and so are
 *  bit-for-bit reproducible, given no FMA contraction (see meson.build).
 *  Accuracy is that of fdlibm: under one ulp.
 */

#pragma once

namespace vsfm {

double DetSin(double x);
double DetCos(double x);
double DetExp(double x);
double DetLog(double x);
double DetPow(double x, double y);

} // namespace vsfm
