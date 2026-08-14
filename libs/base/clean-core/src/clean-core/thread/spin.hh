#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/platform/intrinsics.hh>

namespace cc
{
/// Tell the CPU this iteration is a spin-wait.
/// It yields pipeline resources to the other SMT thread on the same core, and softens the memory-order-violation penalty when the loop finally exits.
/// Not a scheduling yield — the thread stays runnable and nothing is handed to the OS.
/// So this belongs in short bounded spins, never as a substitute for blocking.
///
/// A no-op on architectures with no such hint; correctness must never depend on it.
///
/// The per-compiler plumbing is clean-core/platform/intrinsics.hh, which declares the intrinsic rather than including <immintrin.h> for it.
/// That header is 45 files and 0.25 s; this way is 12 and 0.07 s, and spin.hh sits under mutex.hh, so nearly everything paid it.
CC_FORCE_INLINE void spin_pause()
{
    cc::impl::cpu_pause();
}
} // namespace cc
