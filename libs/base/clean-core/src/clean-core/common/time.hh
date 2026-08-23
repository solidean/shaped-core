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

namespace cc::impl
{
/// A monotonic nanosecond tick, for architectures with no counter register of their own.
///
/// Out of line and through the steady clock, which is what makes it a real call rather than an instruction — the
/// reason `has_cycle_counter()` still says "no cheap counter here" even though `current_cycles()` now works.
[[nodiscard]] u64 monotonic_ticks();
} // namespace cc::impl

/// One wall-clock instant, split into the fields a person reads.
///
/// Only ever produced by local_calendar_time — there is no arithmetic on one and no way back to epoch seconds,
/// because a broken-down local time is an OUTPUT format rather than a way to carry a timestamp around.
struct cc::calendar_time
{
    i32 year = 1970;
    u8 month = 1;  ///< 1-12
    u8 day = 1;    ///< 1-31
    u8 hour = 0;   ///< 0-23
    u8 minute = 0; ///< 0-59
    u8 second = 0; ///< 0-60, since a leap second is a real reading
    u16 millisecond = 0;
};

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

/// Splits epoch seconds into LOCAL calendar fields, applying whatever UTC offset and DST rule is in effect then.
///
/// The offset is resolved per call rather than cached, because a process that outlives a DST transition would
/// otherwise print an hour that never happened — and a long-running one is exactly what logs come from.
/// A reading the platform cannot convert comes back as the epoch rather than as an error: this formats a log line,
/// and a wrong-looking timestamp beats losing the message it was attached to.
[[nodiscard]] cc::calendar_time local_calendar_time(double wall_secs);

/// Whether this architecture has a CHEAP counter register, known at compile time.
/// True on x86 and ARM64, false on WASM.
///
/// **This is no longer "does current_cycles() work" — that always works now.**
/// It is "is reading it free enough for an inner loop": an instruction where this is true, a call through the steady
/// clock where it is false.
/// A benchmark deciding whether to take a reading per iteration wants this; a caller that just needs a monotonic tick
/// does not.
[[nodiscard]] constexpr bool has_cycle_counter()
{
    return impl::has_cycle_counter();
}

/// A monotonic tick, on every platform.
///
/// On x86 this is the TSC; on ARM64 it is CNTVCT_EL0, the generic timer's virtual counter; where there is neither —
/// WASM — it is the steady clock in nanoseconds.
/// All three are constant-rate, so this tracks wall-clock time rather than halted core cycles: a reference clock, not
/// a measure of work done.
/// Nothing here converts it to seconds, because the rate is not knowable without calibration the caller has to do.
///
/// **What a tick IS varies by three orders of magnitude**, which is why the rate is calibrated rather than assumed.
/// A fraction of a nanosecond on x86; about 42 ns on Apple silicon, where the timer runs in the tens of megahertz
/// rather than at the core's clock; a nanosecond on WASM, but through a call rather than an instruction.
/// Fine for a profiling scope everywhere, and only x86 is fine for timing a handful of instructions — a microbenchmark
/// measuring something that short has to loop rather than trust one pair of readings.
///
/// Inline where there is a counter, because a benchmark loop reads this twice per sample and a call would be a
/// meaningful share of what it is trying to measure; a call where there is not, since there is nothing else to do.
[[nodiscard]] CC_FORCE_INLINE u64 current_cycles()
{
    if constexpr (impl::has_cycle_counter())
        return u64(impl::read_cycles());
    else
        return impl::monotonic_ticks();
}

/// current_cycles() plus the core the reading was taken on, or core 0 where the architecture does not report one.
///
/// The core id is the point: without pinning, a thread migrates, and a migration is the usual explanation for a step in
/// otherwise steady per-iteration timings.
/// It costs about ten cycles over current_cycles(), so a caller taking one per event should mean it.
///
/// **Only x86 reports one.** ARM64 has the counter and nothing beside it, so it always says core 0 — a reader must not
/// take that as "the same core every time".
///
/// **Neither reading is ordered against surrounding code on both sides.**
/// This one waits for prior instructions to retire but does not stop later ones from being hoisted above it, so two
/// readings around a very short span can still come back out of order.
[[nodiscard]] CC_FORCE_INLINE u64 current_cycles_and_core(u32& core_out)
{
    if constexpr (!impl::has_cycle_counter())
    {
        core_out = 0;
        return impl::monotonic_ticks();
    }

    unsigned int core = 0;
    auto const cycles = u64(impl::read_cycles_and_core(core));
    core_out = u32(core);
    return cycles;
}
} // namespace cc
