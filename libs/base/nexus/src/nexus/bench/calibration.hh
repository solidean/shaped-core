#pragma once

#include <nexus/bench/fwd.hh>

// The harness measuring its own floor.
//
// Two numbers the engine cannot work without.
// `cc::current_cycles` is a tick counter whose rate varies by three orders of magnitude across architectures, so ticks
// have to be converted to seconds by measuring, never by assuming.
// And a per-iteration time is only meaningful next to what an iteration costs when the body does nothing, which is
// what decides both the batch size and whether the overhead warning fires.

/// What this machine costs the harness, measured once per process.
struct nx::bench::calibration
{
    /// Ticks to seconds, from a short window against the steady clock.
    f64 seconds_per_tick = 0;

    /// Seconds per iteration of a loop whose body is a single `keep`, which is as close to empty as a loop can get and
    /// still exist.
    ///
    /// A genuinely empty body is removed outright and measures zero, which would be a floor no real body could be
    /// compared against.
    f64 empty_iteration_secs = 0;

    /// Seconds for one pair of clock readings, which is the cost a batch amortizes and a pause/resume pair pays.
    f64 clock_pair_secs = 0;

    /// Whether reading the counter is an instruction rather than a call.
    /// False on WASM, where a per-iteration reading would cost more than most bodies.
    bool has_cheap_counter = false;
};

namespace nx::bench
{
/// The calibration for this process, measured on first use and cached.
///
/// Costs a few milliseconds, once.
/// Deliberately lazy rather than done at static init: a binary that never benchmarks never pays it.
[[nodiscard]] calibration const& calibrated();
} // namespace nx::bench
