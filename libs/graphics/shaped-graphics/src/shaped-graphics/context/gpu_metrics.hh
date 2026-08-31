#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

/// How loaded the GPU is, which is the one resource cc cannot answer for.
///
/// CPU, memory, disk and network all come from documented OS calls, so they live in clean-core's platform layer.
/// A GPU does not: there is no portable "how busy is it" syscall anywhere, and even the memory figures come from the
/// graphics API rather than from the OS.
/// So sg owns these outright, and cc never mentions GPUs at all.
///
/// The same three shapes as cc's platform metrics, and for the same reasons.
/// Memory is a **level**, so a plain query answers it.
/// Busy time is a **counter**, so a rate needs `sg::gpu_load_sampler` to hold the previous reading — a bare
/// `query_gpu_load()` returning a percentage could only work by keeping a hidden baseline somewhere, which is what a
/// sampler exists to avoid.
///
/// A load of 1 is the whole device busy, following cc's convention.
/// An unanswerable query returns an error rather than a zero that reads like an idle GPU.

/// GPU memory, as this process sees it.
struct sg::gpu_memory_usage
{
    /// What the OS says THIS process may use right now, which shrinks as other processes take memory.
    /// Not the card's size: see adapter_info::dedicated_video_memory_bytes for that.
    i64 budget_bytes = 0;

    /// What this process is currently using against that budget.
    i64 current_usage_bytes = 0;
};

/// Busy time on one class of GPU engine, summed over every process using it.
///
/// A GPU is several independent engines — 3D, copy, video decode, video encode — and they run at once.
/// Keeping them apart is what makes the total meaningful: adding them would let a device report more than 100% busy,
/// and averaging them would hide a saturated copy engine behind three idle ones.
struct sg::gpu_engine_counter
{
    /// What the platform calls this engine class: "3D", "Copy", "VideoDecode".
    /// Opaque — compare it for equality, do not parse it.
    cc::string engine;

    /// Seconds this engine class has been busy, since the OS started counting.
    /// Two readings difference into a utilization, which is what `gpu_load_sampler` does with them.
    ///
    /// **Summed over the processes currently using the engine, so it is not monotone.**
    /// One of them exiting takes its share of the total with it, and the next reading is lower than the last.
    /// A sampler that sees that reports an error rather than a zero, because the interval carries no utilization it
    /// could name; the reading after it differences normally again.
    f64 busy_secs = 0;
};

/// One reading of the GPU busy counters, per engine class.
struct sg::gpu_counters
{
    cc::vector<sg::gpu_engine_counter> engines;
};

/// How busy the GPU was over one sampling interval.
struct sg::gpu_load
{
    /// What this reading covers, in seconds.
    f64 interval_secs = 0;

    /// **The busiest engine**, in [0, 1].
    ///
    /// Not the sum, which could exceed 1 on a device with four engines, and not the mean, which hides a saturated copy
    /// engine behind three idle ones.
    /// This is what a task manager shows as "GPU %", and it is the number that answers "is the GPU the bottleneck".
    f32 total = 0;

    /// The same reading per engine class, so a caller can see WHICH engine is the busy one.
    /// Points into the sampler, and is invalidated by the next `sample()`.
    cc::span<sg::gpu_engine_load const> per_engine;
};

struct sg::gpu_engine_load
{
    cc::string_view engine;
    f32 busy = 0;
};

/// GPU load, differenced against this sampler's previous reading.
///
/// **Not thread-safe**, and cheap enough that a subsystem wanting its own cadence should hold its own — the same shape
/// and the same reasoning as cc::cpu_load_sampler.
///
/// **Borrows the context**, which must outlive it.
class sg::gpu_load_sampler
{
public:
    /// Takes the baseline immediately, so the first `sample()` covers the time since construction.
    explicit gpu_load_sampler(sg::context const& ctx);

    /// The load since the previous `sample()`, or since construction for the first one.
    /// Errors when the interval carries no load to report — no baseline yet, or a counter that fell because a process
    /// using the GPU exited — and takes a fresh baseline either way, so the next call answers normally.
    [[nodiscard]] cc::result<sg::gpu_load> sample();

    /// Whether this context can answer at all, so a caller decides once instead of probing every frame.
    [[nodiscard]] static bool is_supported(sg::context const& ctx);

private:
    sg::context const* _ctx = nullptr;
    sg::gpu_counters _previous;
    cc::vector<sg::gpu_engine_load> _per_engine;
    f64 _previous_time_secs = 0;
    bool _has_baseline = false;
};
