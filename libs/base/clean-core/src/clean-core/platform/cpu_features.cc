#include <clean-core/platform/cpu_features.hh>
#include <clean-core/platform/impl/cpuid.hh>

using namespace cc::primitive_defines;

namespace cc
{
namespace
{
constexpr bool bit(u32 value, int index)
{
    return (value >> index) & 1u;
}

cpu_features detect()
{
    cpu_features f;

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    auto const l1 = impl::cpuid(1);
    auto const l7 = impl::cpuid(7, 0);
    auto const ext1 = impl::cpuid(0x80000001u);
    auto const xcr0 = impl::xcr0();

    // XCR0 bits 1 and 2 are the SSE and AVX register state; 5, 6 and 7 are the k-masks and the two zmm halves.
    auto const os_saves_ymm = (xcr0 & 0x6) == 0x6;
    auto const os_saves_zmm = os_saves_ymm && (xcr0 & 0xE0) == 0xE0;

    f.sse2 = bit(l1.edx, 26);
    f.sse3 = bit(l1.ecx, 0);
    f.ssse3 = bit(l1.ecx, 9);
    f.cx16 = bit(l1.ecx, 13);
    f.sse41 = bit(l1.ecx, 19);
    f.sse42 = bit(l1.ecx, 20);
    f.movbe = bit(l1.ecx, 22);
    f.popcnt = bit(l1.ecx, 23);
    f.lahf = bit(ext1.ecx, 0);
    f.lzcnt = bit(ext1.ecx, 5);
    f.bmi1 = bit(l7.ebx, 3);
    f.bmi2 = bit(l7.ebx, 8);

    f.avx = os_saves_ymm && bit(l1.ecx, 28);
    f.fma = f.avx && bit(l1.ecx, 12);
    f.f16c = f.avx && bit(l1.ecx, 29);
    f.avx2 = f.avx && bit(l7.ebx, 5);

    f.avx512f = os_saves_zmm && bit(l7.ebx, 16);
    f.avx512dq = f.avx512f && bit(l7.ebx, 17);
    f.avx512cd = f.avx512f && bit(l7.ebx, 28);
    f.avx512bw = f.avx512f && bit(l7.ebx, 30);
    f.avx512vl = f.avx512f && bit(l7.ebx, 31);

    f.x86_64_v2 = f.sse3 && f.ssse3 && f.sse41 && f.sse42 && f.popcnt && f.cx16 && f.lahf;
    f.x86_64_v3 = f.x86_64_v2 && f.avx && f.avx2 && f.fma && f.f16c && f.bmi1 && f.bmi2 && f.lzcnt && f.movbe;
    f.x86_64_v4 = f.x86_64_v3 && f.avx512f && f.avx512bw && f.avx512cd && f.avx512dq && f.avx512vl;
#endif

#if defined(CC_ARCH_ARM64)
    f.neon = true;
#endif

#if defined(__wasm_simd128__)
    f.wasm_simd128 = true;
#endif

    return f;
}
} // namespace

cpu_features const& get_cpu_features()
{
    static cpu_features const features = detect();
    return features;
}
} // namespace cc
