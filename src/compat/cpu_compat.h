/*
 *  VSFilterMod cross-platform port -- runtime CPU feature detection.
 *
 *  Replaces dsutil/vd.h's CCpuID. Only the YUY2 blend in mem_subpic.cpp
 *  still consults it, and its SSE2 and C kernels agree bit for bit, so the
 *  answer affects speed, never output. Off x86 no flag is set.
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
