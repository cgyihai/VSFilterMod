/*
 * Checks the deterministic float helpers CWord::Transform is built on.
 *
 * RcpIntel must return the same bits on every platform, so it is checked
 * against fixed values taken from an Intel CPU's RCPSS. On an Intel x86 host
 * it is additionally compared with the instruction itself over a sweep of
 * inputs. AMD CPUs implement RCPSS differently, so that comparison is skipped
 * there -- which is exactly why the renderer no longer uses the instruction.
 */

#include "compat/det_float.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define HOST_X86 1
#include <xmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace {

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
float Float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

struct Golden { uint32_t in, out; };

const Golden kGolden[] = {
    { 0x3f800000u, 0x3f7ff000u },  // 1
    { 0x3fc00000u, 0x3f2aa000u },  // 1.5
    { 0x40400000u, 0x3eaaa000u },  // 3
    { 0x40e00000u, 0x3e124000u },  // 7
    { 0x42f60000u, 0x3c053000u },  // 123
    { 0x45800800u, 0x397ff000u },  // 4097
    { 0x3a83126fu, 0x4479f800u },  // 0.001
    { 0x3f400000u, 0x3faaa000u },  // 0.75
    { 0x45fff800u, 0x39000800u },  // 8191
    { 0x7149f2cau, 0x0da24000u },  // 1e30
    { 0xc0200000u, 0xbeccc000u },  // -2.5
    { 0x00000000u, 0x7f800000u },  // 0 -> inf
    { 0x80000000u, 0xff800000u },  // -0 -> -inf
    { 0x00000001u, 0x7f800000u },  // denormal -> inf
    { 0x7f800000u, 0x00000000u },  // inf -> 0
    { 0x7f7fffffu, 0x00000000u },  // FLT_MAX -> flushed to 0
};

#ifdef HOST_X86
bool IsIntel()
{
    unsigned int regs[4] = {0, 0, 0, 0};
#if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 0);
    std::memcpy(regs, r, sizeof(regs));
#else
    __get_cpuid(0, &regs[0], &regs[1], &regs[2], &regs[3]);
#endif
    char vendor[13];
    std::memcpy(vendor + 0, &regs[1], 4);   // EBX
    std::memcpy(vendor + 4, &regs[3], 4);   // EDX
    std::memcpy(vendor + 8, &regs[2], 4);   // ECX
    vendor[12] = 0;
    return std::strcmp(vendor, "GenuineIntel") == 0;
}
#endif

} // namespace

int main()
{
    int failures = 0;

    for (const Golden& g : kGolden) {
        const uint32_t got = Bits(vsfm::RcpIntel(Float(g.in)));
        if (got != g.out) {
            std::printf("RcpIntel(%08x) = %08x, expected %08x\n", g.in, got, g.out);
            ++failures;
        }
    }

    struct { float in; int32_t out; } cvt[] = {
        { 1.9f, 1 }, { -1.9f, -1 }, { 2147483520.0f, 2147483520 },
        { 2147483648.0f, INT32_MIN }, { -2147483648.0f, INT32_MIN },
        { -3e9f, INT32_MIN }, { Float(0x7fc00000u), INT32_MIN },
    };
    for (const auto& c : cvt) {
        if (vsfm::CvtTruncSs(c.in) != c.out) {
            std::printf("CvtTruncSs(%g) = %d, expected %d\n",
                        c.in, vsfm::CvtTruncSs(c.in), c.out);
            ++failures;
        }
    }

    if (vsfm::MaxSs(Float(0x7fc00000u), 1000.0f) != 1000.0f ||
        vsfm::MinSs(1.0f, 1.0f) != 1.0f) {
        std::printf("MinSs / MaxSs do not follow MINSS / MAXSS\n");
        ++failures;
    }

#ifdef HOST_X86
    if (IsIntel()) {
        long checked = 0, bad = 0;
        for (uint64_t b = 0; b <= 0xFFFFFFFFull; b += 4093) {
            const float x = Float((uint32_t)b);
            if (x != x) continue;   // NaN payload handling is not needed here
            const uint32_t hw = Bits(_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x))));
            const uint32_t em = Bits(vsfm::RcpIntel(x));
            ++checked;
            if (hw != em) {
                if (bad < 5)
                    std::printf("RCPSS(%08x): hardware %08x, RcpIntel %08x\n",
                                (uint32_t)b, hw, em);
                ++bad;
            }
        }
        std::printf("compared %ld inputs with this Intel CPU's RCPSS: %ld differ\n",
                    checked, bad);
        failures += bad != 0;
    } else {
        std::printf("not an Intel CPU: hardware comparison skipped\n");
    }
#endif

    std::printf(failures ? "FAILED\n" : "ok\n");
    return failures ? 1 : 0;
}
