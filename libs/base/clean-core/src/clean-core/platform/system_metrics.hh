#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>

/// How loaded this machine is right now.
///
/// The live half of cc's system queries, and it comes in two shapes that are not interchangeable.
///
/// A **snapshot** is a level that exists whether or not anyone looks — memory in use, disk free.
/// A plain function answers it, and reading it twice tells you nothing extra.
///
/// A **sampler** reports a rate, and a rate has no value at an instant.
/// Every OS stores CPU time as counters that only ever climb, and a load percentage is the difference of two readings
/// divided by the wall time between them.
/// So something has to hold the previous reading, and here that something is an object the caller owns rather than a
/// hidden process-wide slot that two subsystems would fight over.
///
/// **A load of 1 means the whole machine is busy**, here and everywhere else in cc.
/// Never one core: a process using two cores of thirty-two reports 0.0625, not 2.0.
/// cc::process_cpu_load carries the cores-used figure separately for callers that want it.
///
/// **Absence is normal, and it is never faked.**
/// A query that cannot be answered says so, and says which kind of cannot: never on this platform, refused right now,
/// or attempted and failed.
/// Nothing here returns a zero that reads like a real reading.

/// Why a live query could not answer.
enum class cc::query_status : cc::u8
{
    /// This platform has no such concept, and never will.
    /// A caller can hide the panel once at startup and stop asking.
    unsupported,

    /// The platform has it and would not let us have it — a permission, a counter that would not open.
    denied,

    /// Supported, attempted, did not work this time.
    /// Worth retrying on the next tick, which is what separates it from `unsupported`.
    failed,
};

struct cc::query_error
{
    cc::query_status status = cc::query_status::failed;

    /// What went wrong, for a log line.
    /// Never parse it.
    cc::string detail;
};

/// One live quantity, so a dashboard can decide once at startup what it can draw.
enum class cc::metric : cc::u8
{
    cpu_load,
    memory_usage,
};

/// Monotone CPU time in seconds, as the OS has accumulated it since boot.
///
/// These only ever climb, which is what makes them differenceable.
/// The ratio everyone actually wants is `cc::cpu_load_sampler`'s job; this is the primitive under it, published because
/// a ratio has thrown away the seconds and somebody always wants them back.
struct cc::cpu_counters
{
    f64 user_secs = 0;
    f64 system_secs = 0;
    f64 idle_secs = 0;

    [[nodiscard]] f64 busy_secs() const { return user_secs + system_secs; }
    [[nodiscard]] f64 total_secs() const { return user_secs + system_secs + idle_secs; }
};

struct cc::cpu_counter_set
{
    cc::cpu_counters total;

    /// One entry per logical core, in the OS's own order.
    /// Empty where the platform reports only a total.
    cc::vector<cc::cpu_counters> per_core;
};

/// How busy the CPU was over one sampling interval.
struct cc::cpu_load
{
    /// What this reading actually covers.
    ///
    /// Not decoration: it is what makes the first sample honest rather than a silent lie, and what lets a dashboard on
    /// an irregular cadence draw a correct graph.
    f64 interval_secs = 0;

    /// Busy fraction across the whole machine, in [0, 1].
    f32 total = 0;

    /// The same per logical core, in [0, 1] each.
    ///
    /// **Points into the sampler**, and is invalidated by the next `sample()` or by the sampler's death.
    /// Copy it out if it has to outlive either.
    /// Empty where the platform reports only a total.
    cc::span<f32 const> per_core;
};

/// CPU load, differenced against the previous reading this sampler took.
///
/// **Not thread-safe**, and cheap enough that a subsystem wanting its own cadence should hold its own.
/// That is the point of the type: a free `current_cpu_load()` could only work by keeping one hidden previous reading
/// per process, and two subsystems sampling at different rates would then corrupt each other's numbers.
class cc::cpu_load_sampler
{
public:
    /// Takes the baseline immediately, so the first `sample()` covers the time since construction.
    cpu_load_sampler();

    /// The load since the previous `sample()` on this sampler, or since construction for the first one.
    ///
    /// Two calls in quick succession are honest but useless: the interval is tiny, so the ratio is dominated by the
    /// counters' own granularity.
    /// Read `interval_secs` rather than assuming a cadence.
    [[nodiscard]] cc::result<cc::cpu_load, cc::query_error> sample();

    /// Whether this platform can answer at all, so a caller can decide once instead of probing every tick.
    [[nodiscard]] static bool is_supported();

private:
    cc::cpu_counter_set _previous;
    cc::vector<f32> _per_core;
    f64 _previous_time_secs = 0;
    bool _has_baseline = false;
};

/// Physical memory as it stands right now.
struct cc::memory_usage
{
    i64 total_bytes = 0;

    /// What can still be handed out without pushing something else to disk.
    /// This is the number a dashboard should draw, and it is NOT `total - used` on any modern OS: cache and buffers are
    /// counted as used and are also available.
    i64 available_bytes = 0;

    i64 used_bytes = 0;

    /// The backing store pages get pushed out to, where the OS has one it will describe.
    ///
    /// **Absent on Windows**, which does not report it.
    /// What Windows has instead is the commit charge below, and it is a different quantity — deriving a swap figure
    /// from it produces a number that looks right and is not.
    cc::optional<i64> swap_total_bytes;
    cc::optional<i64> swap_used_bytes;

    /// Windows' commit charge and its limit: memory the system has promised, backed by RAM or by the page file.
    ///
    /// This is what Task Manager labels "Committed", and it is the honest Windows answer to "how close to the edge is
    /// this machine".
    /// Absent everywhere else, where the OS has no such single number.
    cc::optional<i64> commit_limit_bytes;
    cc::optional<i64> commit_used_bytes;

    /// `used_bytes / total_bytes`, or 0 when the total is unknown.
    [[nodiscard]] f32 used_ratio() const;
};

namespace cc
{
/// The raw monotone CPU counters, for a caller that wants to difference them itself.
[[nodiscard]] cc::result<cc::cpu_counter_set, cc::query_error> read_cpu_counters();

/// Physical memory in use, right now.
[[nodiscard]] cc::result<cc::memory_usage, cc::query_error> query_memory_usage();

/// Whether this build on this platform can answer `m` at all.
///
/// Cheap, and stable for the life of the process, so a dashboard builds its panel list once at startup rather than
/// probing every tick and hiding a panel mid-session.
[[nodiscard]] bool is_metric_supported(cc::metric m);
} // namespace cc
