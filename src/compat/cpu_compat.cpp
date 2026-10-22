/*
 *  VSFilterMod cross-platform port -- runtime CPU feature detection.
 */

#include "cpu_compat.h"
#include "simd_compat.h"

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define VSFM_CPU_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

CCpuID g_cpuid;

CCpuID::CCpuID() : m_flags(0)
{
#if defined(VSFM_CPU_X86)

#if defined(_MSC_VER)
    int regs[4] = { 0, 0, 0, 0 };
    __cpuid(regs, 0);
    if (regs[0] >= 1) {
        __cpuid(regs, 1);
        const unsigned edx = (unsigned)regs[3];
        if (edx & (1u << 23)) m_flags |= mmx;
        if (edx & (1u << 25)) m_flags |= ssemmx | ssefpu;
        if (edx & (1u << 26)) m_flags |= sse2;
    }
#else
    unsigned eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (edx & (1u << 23)) m_flags |= mmx;
        if (edx & (1u << 25)) m_flags |= ssemmx | ssefpu;
        if (edx & (1u << 26)) m_flags |= sse2;
    }
#endif

#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(VSFM_HAVE_SSE2)
    /* sse2neon maps the SSE2 kernels onto NEON, which every ARMv8 core has.
     * Without that header VSFM_HAVE_SSE2 is undefined and the flag stays
     * clear, so Rasterizer takes its scalar path. */
    m_flags |= sse2;
#endif
}
