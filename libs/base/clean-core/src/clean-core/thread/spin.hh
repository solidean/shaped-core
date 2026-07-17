#pragma once

#include <clean-core/common/macros.hh>

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
#if defined(CC_COMPILER_MSVC)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#endif

namespace cc
{
/// Tell the CPU this iteration is a spin-wait: it yields pipeline resources to the other SMT thread on the same
/// core and softens the memory-order-violation penalty when the loop finally exits. Not a scheduling yield —
/// the thread stays runnable and nothing is handed to the OS, so this belongs in short bounded spins, never as
/// a substitute for blocking.
///
/// A no-op on architectures with no such hint; correctness must never depend on it.
CC_FORCE_INLINE void spin_pause()
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    _mm_pause();
#elif defined(CC_ARCH_ARM64) || defined(CC_ARCH_ARM32)
#if defined(CC_COMPILER_MSVC)
    __yield();
#else
    __asm__ __volatile__("yield");
#endif
#endif
}
} // namespace cc
