#pragma once

#include <clean-core/string/string.hh>
#include <nexus/bench/fwd.hh>

// What the machine was, and what was done to it.
//
// A benchmark number is a statement about a machine, and a number without the machine is not interpretable.
// Reporting is cheap and always worth it; CONTROLLING the machine is platform-specific, often needs privileges, and is
// where benchmarking harnesses go to die — so this reports thoroughly and controls almost nothing.

/// What this run was measured on.
///
/// **Provisional.** shaped-core has no system-information library yet, so the fields a real one would fill say
/// `unknown` rather than being absent: the shape is what downstream tooling reads, and a schema that grows fields
/// later would break every consumer written against it.
/// `is_provisional` is what says so in the output, rather than leaving a reader to notice.
struct nx::bench::system_summary
{
    cc::string os;
    cc::string arch;

    /// The CPU model, or `unknown` until there is something to ask.
    cc::string cpu;

    /// Hardware threads the OS reports, which is the one hardware fact reachable today.
    isize logical_cores = 0;

    /// The build this binary is, and whether it is measuring its own assertions.
    ///
    /// `CC_ASSERT_ENABLED` decides the second, and it is the difference between benchmarking `cc::vector` and
    /// benchmarking its bounds checks — the single most common way a number here is quietly wrong.
    cc::string build;
    bool assertions_enabled = false;

    /// True while any field above is a placeholder, which is every run until sysinfo lands.
    bool is_provisional = true;
};

/// A reading of how busy the machine is, taken before and after a run.
///
/// The purpose is a WARNING rather than a measurement: if the box was busy, the reader should know before trusting
/// anything.
struct nx::bench::load_sample
{
    /// Reference ticks per nanosecond, over a short window.
    ///
    /// A better signal than load average for this purpose, and it needs no OS API at all: it measures what actually
    /// happened to the core the benchmark ran on.
    /// A ratio that moves between the two brackets means the clock changed or contention appeared, which is exactly
    /// the condition worth warning about.
    f64 ticks_per_ns = 0;

    /// The share of every core busy across the machine, or negative where the platform cannot say.
    ///
    /// What a reader expects to see, and it catches what the tick ratio does not: another process saturating a
    /// different core changes nothing about this thread's clock and changes a great deal about a cache.
    f64 cpu_busy_fraction = -1;
};

namespace nx::bench
{
/// The system summary, gathered once and cached.
[[nodiscard]] system_summary const& describe_system();

/// A load reading, taken over a couple of milliseconds.
[[nodiscard]] load_sample sample_load();

/// Pin the calling thread to one core for the duration, and report whether it worked.
///
/// **Off unless asked for**, and that is deliberate rather than timid: a harness that silently pins to core 0 will
/// eventually pin onto the core something else is already using, and the resulting measurement is worse than the
/// unpinned one.
///
/// Implemented on Windows and Linux; everywhere else this reports false and changes nothing.
/// macOS offers only advisory affinity hints, which cannot honour this contract.
[[nodiscard]] bool try_pin_to_core(int core);

/// Undo a successful pin.
void unpin();
} // namespace nx::bench
