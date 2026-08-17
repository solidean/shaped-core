#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/fwd.hh>
#include <clean-core/platform/intrinsics.hh>

// The timing seam: what everything in shaped-core should reach for instead of <chrono>.
//
// <chrono> is the most expensive header in the standard library on MSVC — 1.16 s and 148 files entered,
// because it drags in <format> and the whole <xutility> chain behind it.
// Only time.cc is allowed to include it, and the .shaped-lint.yml entry next to that file is what enforces it.
// docs/notes/build-times.md has the measurement.

namespace cc
{
/// Seconds on a steady clock: never adjusted, never runs backwards, and its zero point is arbitrary.
/// So a single reading is meaningless and only a difference of two is.
/// That is what makes it the right clock for measuring how long something took.
///
/// This IS the monotonic clock; there is no separate coarser or finer one to ask for.
/// For a wall-clock timestamp — a date, a persisted expiry — this is the wrong function; current_time_wall_secs is.
[[nodiscard]] double current_time_steady_secs();

/// Seconds since the Unix epoch (1970-01-01 00:00:00 UTC).
/// Comparable ACROSS processes and across runs, which is what makes this the one to persist — a steady reading is not.
///
/// It can step, forwards or backwards, whenever the system clock is set or an NTP correction lands.
/// So a difference of two readings is NOT a duration: for how long something took, use current_time_steady_secs.
/// A double holds present-day epoch seconds to about a microsecond, finer than any platform's wall clock resolves.
[[nodiscard]] double current_time_wall_secs();

/// Whether current_cycles() returns a real reading on this architecture, known at compile time.
/// False on ARM and WASM, where a caller must fall back to current_time_steady_secs().
[[nodiscard]] constexpr bool has_cycle_counter()
{
    return impl::has_cycle_counter();
}

/// A monotonic cycle count, or 0 where the architecture has none (see has_cycle_counter).
///
/// On x86 this is the TSC.
/// It is constant-rate on modern CPUs, so it tracks wall-clock time rather than halted core cycles — a cheap reference clock, not a measure of work done.
/// Nothing here converts it to seconds, because the rate is not knowable without calibration the caller has to do.
///
/// Inline and header-only on purpose: a benchmark loop reads this twice per sample, and a call would be a
/// meaningful share of what it is trying to measure.
[[nodiscard]] CC_FORCE_INLINE u64 current_cycles()
{
    return u64(impl::read_cycles());
}
} // namespace cc
