/*
 *  VSFilterMod cross-platform port -- deterministic single-precision helpers.
 *
 *  CWord::Transform used to have two implementations: an SSE one (single
 *  precision, with an approximate reciprocal) taken on x86, and on AArch64
 *  when the sse2neon header was found, and a double-precision C one taken
 *  otherwise. They disagree by whole pixels under \distort and draw \rnd
 *  jitter in a different order, so the same script rendered differently
 *  depending on the CPU and on whether one optional header was installed.
 *
 *  There is now a single scalar implementation that performs exactly the SSE
 *  path's IEEE operations, in the same order, on plain floats. Additions,
 *  multiplications, divisions and comparisons are correctly rounded on every
 *  platform we build for, so the result is the same everywhere -- provided
 *  the compiler does not fuse a * b + c (meson.build passes
 *  -ffp-contract=off, and RTS.cpp turns contraction off by pragma for MSVC
 *  and Clang as well) and
 *  float arithmetic is not evaluated in extended precision (x87; the 32-bit
 *  x86 build is compiled for SSE math).
 *
 *  The three operations SSE does not define the IEEE way are emulated here.
 */

#pragma once

#include <cstdint>
#include <climits>

namespace vsfm {

/* RCPSS / RCPPS: a 12-bit approximation of 1/x whose exact value is
 * implementation specific -- Intel and AMD CPUs return different results,
 * and sse2neon computes yet another. This reproduces Intel's bit for bit (it
 * was checked against the hardware for all 2^32 inputs), so output matches
 * upstream VSFilterMod as rendered on an Intel machine, on every CPU. */
float RcpIntel(float x);

/* CVTTSS2SI: truncation toward zero; NaN and anything out of int32 range
 * become INT32_MIN. A plain C++ cast is undefined there and AArch64
 * saturates instead. */
inline int32_t CvtTruncSs(float f)
{
    return (f >= -2147483648.0f && f < 2147483648.0f) ? (int32_t)f : INT32_MIN;
}

/* MINPS / MAXPS: the second operand is returned when either is NaN or when
 * they compare equal, which is not what std::min / std::max do. */
inline float MinSs(float a, float b) { return a < b ? a : b; }
inline float MaxSs(float a, float b) { return a > b ? a : b; }

} // namespace vsfm
