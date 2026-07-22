#pragma once

#include <clean-core/common/macros.hh>
#include <nexus/bench/fwd.hh> // for the cc primitive aliases in nx::bench

#include <chrono>

// The always-on measurement baseline: a monotonic nanosecond clock and a cheap reference-cycle counter.
// Every backend reuses these so elapsed time and cycles are produced even with no PMU access at all.
//
// TODO(clean-core): candidate to promote — clean-core has no timing/cycle API yet
// (see libs/base/nexus/docs/stdlib-migration.md). Kept local to nexus/bench for now.

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
#if defined(CC_COMPILER_MSVC)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

namespace nx::bench::impl
{
/// Whether read_reference_cycles() returns a real value on this architecture.
CC_FORCE_INLINE bool has_reference_cycles()
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    return true;
#else
    return false;
#endif
}

/// A monotonic reference cycle count. On x86 this is the TSC: constant-rate on modern CPUs, so it tracks
/// wall-clock time rather than halted core cycles — a baseline, not the PMU cycle event. Returns 0 where no
/// cheap counter exists; callers then mark the reference_cycles sample invalid.
CC_FORCE_INLINE u64 read_reference_cycles()
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    return u64(__rdtsc());
#else
    return 0;
#endif
}

/// Nanoseconds from the repo-blessed monotonic clock.
inline u64 steady_now_ns()
{
    auto const since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}
} // namespace nx::bench::impl
