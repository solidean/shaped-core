#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>

/// What machine this is: CPU topology, caches, memory and OS, gathered once and reused.
///
/// This is the DESCRIPTION half of cc's system queries, and it is one of three concepts that are deliberately kept apart.
/// A description is what cannot change while the process runs.
/// A snapshot is every level-valued reading at one instant, and a sampler is what changed since last time — both in
/// platform/system_metrics.hh, because a rate has no value at an instant.
///
/// **Nothing in cc::system_info may change while the process runs, and there is no refresh function.**
/// That is what lets `get_system_info()` hand back a reference callers keep forever.
/// The one relaxation is named in the field: a mutable value may be captured if its name says WHEN, as
/// `timezone_at_start` does.
/// That suffix is for environment identity, never for a measurement — a `cpu_frequency_at_start` would violate the rule
/// it appears to follow.
///
/// **Never synthesize a plausible value.**
/// A field the platform cannot answer stays empty rather than defaulting, because a zero-filled description reads as a
/// working one and nobody looks twice.
///
/// This carries no hostname, no user name and no machine id — those are personal data and live behind an explicit
/// request in platform/system_identifier.hh.

enum class cc::cache_kind : cc::u8
{
    data,
    instruction,
    unified,
};

/// One level of one core class's cache hierarchy.
struct cc::cpu_cache_level
{
    i32 level = 0; ///< 1, 2, 3
    cc::cache_kind kind = cc::cache_kind::unified;
    i64 size_bytes = 0;
    i32 line_size_bytes = 0;

    /// How many logical cores share ONE instance of this level.
    /// A size means nothing without it: an 8 MiB L2 shared by four cores is a different machine from one that is not.
    i32 sharing_cores = 0;
};

/// One group of cores that are alike, which on any recent CPU is fewer than all of them.
///
/// Performance and efficiency cores differ in clock, in cache and in whether they carry SMT at all, and a single core
/// count cannot say which of them a thread landed on.
/// A machine that really is homogeneous has exactly one of these.
struct cc::cpu_core_class
{
    /// The vendor's word for this class where there is one — "performance", "efficiency" — and empty otherwise.
    cc::string name;

    i32 physical_cores = 0;
    i32 logical_cores = 0;

    cc::optional<i64> base_clock_hz;
    cc::optional<i64> boost_clock_hz;

    cc::vector<cc::cpu_cache_level> caches;
};

struct cc::numa_node
{
    i32 index = 0;
    cc::optional<i64> memory_bytes;
};

/// The machine, as it is for this process's whole life.
///
/// Obtained from `cc::get_system_info()`, which computes it once.
/// Every field is empty rather than wrong where the platform cannot answer.
struct cc::system_info
{
    // --- CPU ---

    cc::string cpu_brand;        ///< "AMD Ryzen 9 7950X 16-Core Processor"
    cc::string cpu_vendor;       ///< "AuthenticAMD", "GenuineIntel", "Apple"
    cc::string cpu_architecture; ///< "x64", "arm64", "wasm32" — what this binary was built for

    /// One entry per group of alike cores, in descending order of performance.
    /// Empty where the platform reports no topology at all.
    cc::vector<cc::cpu_core_class> core_classes;

    cc::vector<cc::numa_node> numa_nodes;

    // --- memory ---

    cc::optional<i64> ram_total_bytes;

    /// Configured transfer rate in MT/s, which is the number a memory module is sold by.
    /// Rarely available outside Windows, and never on a machine that hides its SMBIOS.
    cc::optional<i64> ram_speed_mts;

    cc::optional<i64> page_size_bytes;

    // --- OS ---

    cc::string os_name;        ///< "Windows", "Linux", "macOS"
    cc::string os_version;     ///< "11", "6.8.0", "14.4"
    cc::string os_build;       ///< the build or kernel revision, where the OS distinguishes one
    cc::string kernel_version; ///< the full kernel string, unparsed

    /// Wall-clock seconds since the epoch at which this machine booted.
    /// Immutable for the process's life even though it names an instant, which is why it is here and `uptime_secs()` is
    /// derived rather than stored.
    f64 boot_time_wall_secs = 0;

    /// The timezone and locale as they were when this was first computed.
    /// Both can change under a running process; the suffix is the claim that these are the startup values.
    /// cc offers no query for the current ones.
    cc::string timezone_at_start;
    cc::string locale_at_start;

    // --- derived totals ---
    //
    // Accessors rather than fields, so the flat number and the topology can never disagree.

    /// Hardware threads across every core class.
    ///
    /// This is the MACHINE's count, never clamped to what this process may use.
    /// A thread pool wants `cc::recommended_worker_count()` in platform/resource_limits.hh instead — inside a container
    /// the two differ by more than an order of magnitude, and this one is the wrong answer.
    [[nodiscard]] i32 logical_cores() const;

    [[nodiscard]] i32 physical_cores() const;

    /// Seconds since boot, derived from `boot_time_wall_secs` and the current wall clock.
    [[nodiscard]] f64 uptime_secs() const;

    /// The largest cache of `level` across every core class, or nothing where no class reports that level.
    [[nodiscard]] cc::optional<i64> largest_cache_bytes(i32 level) const;
};

namespace cc
{
/// The machine description, computed on the first call and reused by every later one.
///
/// The first call reads the OS and allocates; every later call is a load.
/// Thread-safe, and the returned reference stays valid for the rest of the process.
[[nodiscard]] cc::system_info const& get_system_info();
} // namespace cc
