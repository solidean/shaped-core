#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/hardware_counters.hh>

// Internal interface between the platform-agnostic front (hardware_counters.cc) and the per-OS backends
// (hardware_counters_{linux,windows,none}.cc). Exactly one backend .cc is compiled, selected in CMake.
//
// The backend owns everything platform-specific: which counters the CPU exposes, their native names, and
// the actual measurement. The front owns the portable vocabulary — the descriptions, the default set, and
// the printing. The baseline (elapsed_nanoseconds, and reference_cycles where cheap) is the backend's job
// too, so it is always present regardless of PMU access.

namespace nx::bench::impl
{
/// One counter as the platform sees it: its logical id, the native name, and whether it is measurable now.
struct backend_counter
{
    hw_counter id;
    cc::string native_name; ///< empty means "no distinct native name" — the front falls back to a logical one
    bool available = false;
};

/// Enumerate what the current platform/CPU can measure right now. Baseline counters are always included
/// (available); PMU counters appear with `available == false` when present-but-not-readable.
cc::vector<backend_counter> backend_enumerate_counters();

/// Measure `counters` across a single invocation of `body`, returning one sample per requested counter in
/// the same order. Fully populates each sample (id, native name, value, valid). Baseline counters are
/// always valid; PMU counters degrade to `valid == false` when they cannot be read.
cc::vector<hw_counter_sample> backend_measure(cc::function_ref<void()> body, cc::span<hw_counter const> counters);

/// The portable fallback name for a counter, used when a backend has no distinct native name to report
/// (defined by the front so every backend spells the baseline counters the same way).
cc::string_view logical_counter_name(hw_counter c);

/// Emit a one-time warning (process-wide, thread-safe) that full hardware counters are unavailable and how
/// to enable them. Backends call this the first time a requested PMU counter degrades. No-op after the
/// first call. `platform_hint` is appended so each OS can point at its own setup step.
void warn_pmu_unavailable_once(cc::string_view platform_hint);
} // namespace nx::bench::impl
