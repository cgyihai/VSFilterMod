/*
 *  VSFilterMod cross-platform port -- SIMD intrinsics front-end.
 *
 *  Rasterizer.cpp's blend kernels are written against SSE2. Rather than fork
 *  them per architecture, AArch64 pulls in sse2neon, which implements the same
 *  intrinsic names on NEON. Everything else falls back to the scalar kernels
 *  that already exist alongside them (Rasterizer branches on g_cpuid).
 */

#pragma once

#include "platform.h"

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)

#define VSFM_HAVE_SSE2 1
#include <xmmintrin.h>
#include <emmintrin.h>
/* MMX is only reachable on 32-bit x86; the 64-bit paths use SSE2 throughout. */
#if defined(__i386__) || defined(_M_IX86)
#define VSFM_HAVE_MMX 1
#include <mmintrin.h>
#endif

#elif defined(__aarch64__) || defined(_M_ARM64)

/* sse2neon is header-only and optional. Without it the build still works --
 * g_cpuid reports no SSE2 and Rasterizer uses the scalar kernels that sit
 * alongside the SSE2 ones -- it is just slower. */
#if defined(__has_include)
#if __has_include(<sse2neon.h>)
#define VSFM_HAVE_SSE2 1
#define SSE2NEON_SUPPRESS_WARNINGS 1
#include <sse2neon.h>
#endif
#endif

#else

/* No SIMD: g_cpuid reports no SSE2 and Rasterizer takes the scalar path. */

#endif

/* MSVC spelling used throughout the rasterizer. */
#if !defined(_MSC_VER)
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#endif
