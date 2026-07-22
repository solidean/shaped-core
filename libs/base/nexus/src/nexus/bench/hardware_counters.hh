#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/fwd.hh>

// Hardware performance counters, miniperf-style.
//
// measure_hw_counters(body) invokes `body` EXACTLY ONCE with the requested counters running, and returns
// the deltas. If you want repetition, loop inside `body` yourself — nexus does not loop for you. The call is
// best-effort and never fails as a whole: it always yields at least elapsed time (and, on x86, a cycle
// count); counters the machine cannot read this run come back with `valid == false` rather than erroring.
//
// Full PMU counters (instructions/branches/caches) need hardware and OS support that is not always present:
//   - Linux: gated by /proc/sys/kernel/perf_event_paranoid; blocked inside many sandboxes/containers.
//   - Windows: needs elevation (discouraged) or the standard non-admin profiling permissions — run
//     tools/setup-pmu-access.ps1 to grant them (a PMU-backed ETW session may still need extra per-session
//     ACLs). Without access, only the baseline is produced and nexus prints a single warning once.
//   - macOS/ARM64: unsupported for now (baseline only).
// Query available_hw_counters() to see what THIS machine reports as measurable right now.
//
// NX_BENCH_HAS_HW_COUNTERS (1/0) says whether a real PMU backend was compiled in at all; it is a coarse
// compile-time hint, not a runtime capability — always trust available_hw_counters() for the latter.

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
/// PMU counters appear with available == false when the CPU exposes them but the current process may not
/// read them (privilege/sandbox) — call it after any setup step to see the effect.
[[nodiscard]] cc::vector<hw_counter_info> available_hw_counters();

/// The sensible default counter set used when measure_hw_counters() is called without an explicit list.
[[nodiscard]] cc::span<hw_counter const> default_hw_counter_set();

/// Print the available counters and their descriptions to stdout (human-readable, for quick discovery).
void print_hw_counters();

/// Measure the default counter set across a single invocation of `body`.
[[nodiscard]] hw_measurement measure_hw_counters(cc::function_ref<void()> body);

/// Measure `counters` across a single invocation of `body`. Unavailable counters come back invalid.
[[nodiscard]] hw_measurement measure_hw_counters(cc::function_ref<void()> body, cc::span<hw_counter const> counters);
} // namespace nx::bench
