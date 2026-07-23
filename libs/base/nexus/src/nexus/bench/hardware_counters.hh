#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/fwd.hh>

// Hardware performance counters, miniperf-style.
//
// measure_hw_counters(body) invokes `body` once with the requested counters running, and returns the deltas.
// The exception is hw_measure_config::measure_all, which re-runs `body` once per counter subset (see below).
// For repetition within a single measurement, loop inside `body` yourself — nexus does not loop for you.
// Best-effort and never fails as a whole: it always yields at least elapsed time (and, on x86, a cycle count).
// A counter the machine cannot read this run comes back with `valid == false` rather than erroring.
//
// Full PMU counters (instructions/branches/caches) need hardware and OS support that is not always present:
//   - Linux: gated by /proc/sys/kernel/perf_event_paranoid; blocked inside many sandboxes/containers.
//   - Windows: read via a real-time ETW context-switch session, which needs a one-time non-admin grant (below).
//   - macOS/ARM64: unsupported for now (baseline only).
// The Windows grant: run tools/setup-pmu-access.ps1 once (elevated) for Performance Log Users + SeSystemProfilePrivilege + the ETW session-GUID ACLs, then sign out and back in.
// Without any access only the baseline is produced, and nexus prints a single warning once.
// Query available_hw_counters() to see what THIS machine reports as measurable right now.
//
// NX_BENCH_HAS_HW_COUNTERS (1/0) says whether a real PMU backend was compiled in — a coarse compile-time hint, not a runtime capability.
// Always trust available_hw_counters() for the latter.

namespace nx::bench
{
/// A counter this machine can measure right now, with its native name and our best-effort description.
struct hw_counter_info
{
    hw_counter id;
    cc::string name;        ///< native/platform name (may be cryptic, e.g. "BranchMispredictions")
    cc::string description; ///< our best-effort explanation of what it counts
    bool available = false; ///< whether this counter is measurable right now (PMU + privilege permitting)
};

/// One counter value measured across a single run.
struct hw_counter_sample
{
    hw_counter id;
    cc::string name;
    u64 value = 0;
    bool valid = false; ///< false if this counter could not be read this run (then `value` is meaningless)
};

/// The result of one measure_hw_counters() call: one sample per requested counter, in request order.
struct hw_measurement
{
    cc::vector<hw_counter_sample> samples;

    /// The measured value for `c`, or nullopt if it was not requested or came back invalid.
    [[nodiscard]] cc::optional<u64> value_of(hw_counter c) const;
};

/// The hardware counters this machine reports as measurable right now, in a stable order.
///
/// Always includes the baseline (elapsed_nanoseconds, and reference_cycles where a cheap counter exists).
/// PMU counters appear with available == false when the CPU exposes them but the current process cannot read them (privilege/sandbox).
/// Call it after any setup step to see the effect.
[[nodiscard]] cc::vector<hw_counter_info> available_hw_counters();

/// The sensible default counter set used when measure_hw_counters() is called without an explicit list.
[[nodiscard]] cc::span<hw_counter const> default_hw_counter_set();

/// Print the available counters and their descriptions to stdout (human-readable, for quick discovery).
void print_hw_counters();

/// Options for measure_hw_counters().
struct hw_measure_config
{
    /// Which counters to measure. Absent (nullopt) means the default set (default_hw_counter_set()).
    cc::optional<cc::vector<hw_counter>> counters;

    /// Measure EVERY requested counter, even when they exceed the hardware's simultaneous-counter budget.
    /// Only a few PMU counters can be programmed at once, so a single pass silently drops the rest (invalid).
    /// With this set, `body` is invoked once per fitting subset and the results are combined — nothing is left unmeasured.
    /// The body must be repeatable and deterministic for the combined values to be comparable.
    /// It runs once per pass, so a slow body costs proportionally more.
    /// The always-on baseline (elapsed/cycles) is taken from the first pass.
    /// No effect when the requested counters already fit in one pass.
    bool measure_all = false;
};

/// Measure counters across invocation(s) of `body`.
/// With the default config `body` runs exactly once, and counters that do not fit the hardware budget come back invalid.
/// See hw_measure_config to override the counter set, or to measure everything across multiple passes.
[[nodiscard]] hw_measurement measure_hw_counters(cc::function_ref<void()> body, hw_measure_config const& config = {});
} // namespace nx::bench
