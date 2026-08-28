#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/platform/system_metrics.hh> // cc::query_error

/// What THIS process is consuming.
///
/// Much cheaper and much more portable than asking about the machine: a process asking about itself needs no
/// permission anywhere.
/// Asking about ANOTHER process is where permissions, enumeration and platform divergence begin, and that is deliberately
/// not here yet.
///
/// **Every query takes a process id even though only `cc::current_process` works today.**
/// That one word at each call site is what makes widening this later a permission story rather than a rename of
/// everything, and it is honest about what the function is asking.
/// Passing any other id reports `unsupported` rather than asserting, because it is a capability that does not exist yet
/// rather than a caller mistake.
///
/// The same two shapes as platform/system_metrics.hh: memory is a level and a plain query answers it, while CPU time is
/// a monotone counter that needs `cc::process_cpu_sampler` to become a rate.

/// A process, for the queries below.
///
/// An opaque handle rather than a raw pid: the OS's own numbering differs per platform, and nothing here wants a caller
/// doing arithmetic on it.
enum class cc::process_id : cc::u64
{
};

namespace cc
{
/// The calling process, and today the only id these queries answer for.
inline constexpr cc::process_id current_process = cc::process_id(0);
} // namespace cc

/// Memory and handles as they stand right now.
struct cc::process_usage
{
    /// Physical memory this process currently occupies — the working set, or RSS.
    i64 resident_bytes = 0;

    /// The highest `resident_bytes` ever reached, as the OS tracked it.
    ///
    /// **Read from the OS rather than derived from samples**, and that is the point of having it.
    /// No sampling cadence can reconstruct a true peak: a spike between two samples simply never happened as far as the
    /// samples are concerned.
    i64 peak_resident_bytes = 0;

    /// Committed memory private to this process: not shared with another, and not merely reserved.
    ///
    /// **Not the virtual address space**, which is mostly reserved-and-untouched and says nothing useful — a process
    /// holding 14 MiB can reserve terabytes of it.
    /// This is the number a task manager shows as the process's memory cost: Windows' commit size, Linux's VmData,
    /// Darwin's physical footprint.
    /// Those are not literally the same quantity, and each is its platform's closest honest answer to the same
    /// question.
    i64 private_bytes = 0;

    i32 thread_count = 0;

    /// Open OS handles — Windows handles, POSIX file descriptors.
    /// Absent where counting them would cost a directory walk the caller did not ask for.
    cc::optional<i32> open_handles;
};

/// Monotone per-process counters, in seconds and bytes since the process started.
struct cc::process_cpu_counters
{
    f64 user_secs = 0;
    f64 kernel_secs = 0;

    [[nodiscard]] f64 total_secs() const { return user_secs + kernel_secs; }

    /// Bytes this process has moved through the I/O system, where the OS accounts for it.
    /// Includes cached reads on every platform that reports it at all, so it is traffic rather than disk traffic.
    cc::optional<i64> bytes_read;
    cc::optional<i64> bytes_written;

    cc::optional<i64> page_faults;
};

/// How much CPU this process used over one sampling interval.
struct cc::process_cpu_load
{
    f64 interval_secs = 0;

    /// **How many cores' worth**, so 2.0 means two cores kept fully busy.
    ///
    /// Deliberately not a percentage.
    /// The two conventions in the wild disagree — a Unix `top` shows 400% for this, a Windows task manager shows 12.5%
    /// on a 32-core box — and a bare "percent" field would be read as whichever one the reader is used to.
    /// A count of cores means the same thing to both.
    f32 cores_used = 0;

    /// `cores_used` divided by the machine's logical cores, in [0, 1].
    /// The number a bar chart wants, and 0 where the core count is unknown.
    f32 machine_fraction = 0;
};

/// Per-process CPU load, differenced against this sampler's previous reading.
///
/// **Not thread-safe.** Same shape and the same reasoning as cc::cpu_load_sampler.
class cc::process_cpu_sampler
{
public:
    explicit process_cpu_sampler(cc::process_id id = cc::current_process);

    [[nodiscard]] cc::result<cc::process_cpu_load, cc::query_error> sample();

    [[nodiscard]] static bool is_supported();

private:
    cc::process_cpu_counters _previous;
    cc::process_id _id = cc::current_process;
    f64 _previous_time_secs = 0;
    bool _has_baseline = false;
};

namespace cc
{
[[nodiscard]] cc::result<cc::process_usage, cc::query_error> query_process_usage(cc::process_id id = cc::current_process);

[[nodiscard]] cc::result<cc::process_cpu_counters, cc::query_error> read_process_cpu_counters(cc::process_id id
                                                                                              = cc::current_process);
} // namespace cc
