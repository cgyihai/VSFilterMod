/*
 *  VSFilterMod cross-platform port -- SIMD intrinsics front-end.
 *
 *  Rendering does not depend on SIMD: the rasterizer's blend kernels and the
 *  glyph transform are plain C that every platform runs, so output is the
 *  same on every CPU. The one SSE2 kernel left -- YUY2 blending in
 *  mem_subpic.cpp, used by TextSubMod -- has a C twin that computes the same
 *  bytes for every input, and is used only where SSE2 is native.
 */

#pragma once

#include "platform.h"

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)

#define VSFM_HAVE_SSE2 1
#include <xmmintrin.h>
#include <emmintrin.h>

#endif
