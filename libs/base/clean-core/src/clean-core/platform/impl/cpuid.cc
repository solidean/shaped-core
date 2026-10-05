#include <clean-core/platform/impl/cpuid.hh>

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)

#if defined(CC_COMPILER_MSVC)
#include <intrin.h> // __cpuidex, _xgetbv
#else
#include <cpuid.h> // __cpuid_count
#endif

using namespace cc::primitive_defines;

namespace cc::impl
{
namespace
{
cpuid_registers raw_cpuid(u32 leaf, u32 subleaf)
{
    cpuid_registers r;
#if defined(CC_COMPILER_MSVC)
    int out[4] = {};
    __cpuidex(out, int(leaf), int(subleaf));
    r.eax = u32(out[0]);
    r.ebx = u32(out[1]);
    r.ecx = u32(out[2]);
    r.edx = u32(out[3]);
#else
    __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
#endif
    return r;
}
} // namespace

cpuid_registers cpuid(u32 leaf, u32 subleaf)
{
    // The basic and extended ranges each report their own maximum in leaf 0 / 0x80000000.
    auto const range = leaf & 0x80000000u;
    if (raw_cpuid(range, 0).eax < leaf)
        return {};
    return raw_cpuid(leaf, subleaf);
}

u64 xcr0()
{
    // XGETBV faults unless the OS has set CR4.OSXSAVE, which CPUID reports as leaf 1, ECX bit 27.
    if ((cpuid(1).ecx & (1u << 27)) == 0)
        return 0;
#if defined(CC_COMPILER_MSVC)
    return _xgetbv(0);
#else
    // Inline asm rather than _xgetbv, which clang and GCC refuse outside a function compiled with -mxsave.
    u32 lo = 0;
    u32 hi = 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (u64(hi) << 32) | lo;
#endif
}
} // namespace cc::impl

#endif
