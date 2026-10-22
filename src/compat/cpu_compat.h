/*
 *  VSFilterMod cross-platform port -- runtime CPU feature detection.
 *
 *  Replaces dsutil/vd.h's CCpuID. Rasterizer::Draw branches on g_cpuid at
 *  runtime, so the flag has to reflect the machine, not the compiler flags.
 *
 *  On AArch64 the SSE2 paths are compiled through sse2neon (see simd_compat.h),
 *  so the flag is reported as set: the "SSE2" branch is the one we want there.
 */

#pragma once

#include "platform.h"

class CCpuID
{
public:
    enum flag_t
    {
        mmx   = 1 << 0,
        ssemmx = 1 << 1,
        ssefpu = 1 << 2,
        sse2  = 1 << 3,
        _3dnow = 1 << 4,
    };

    CCpuID();

    unsigned m_flags;
};

extern CCpuID g_cpuid;
