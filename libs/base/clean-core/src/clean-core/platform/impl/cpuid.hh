#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/fwd.hh>

// The one place clean-core executes CPUID and XGETBV, so the intrinsics headers that spell them are included once.
// x86 only: on every other architecture neither function is declared.

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)

namespace cc::impl
{
struct cpuid_registers
{
    u32 eax = 0;
    u32 ebx = 0;
    u32 ecx = 0;
    u32 edx = 0;
};

/// CPUID for `leaf` and `subleaf`, or all-zero registers for a basic or extended leaf above the CPU's maximum.
/// Checking the maximum first is what makes a zero meaningful: an unsupported leaf otherwise returns the highest
/// basic leaf's data on Intel, which reads as features the CPU does not have.
[[nodiscard]] cpuid_registers cpuid(u32 leaf, u32 subleaf = 0);

/// XCR0, the register-state mask the OS saves on a context switch, or 0 when the OS has not enabled XSAVE.
/// A CPU with AVX whose OS does not save ymm cannot use it, so every AVX-family feature is gated on this.
[[nodiscard]] u64 xcr0();
} // namespace cc::impl

#endif
