#pragma once

#include <clean-core/record/desc.hh>
#include <clean-core/record/fwd.hh>

// Sampling: what the threads were actually doing, as opposed to what they were instrumented to say.
//
// Instrumentation answers "how long did the thing I named take" and is blind to everything nobody named.
// A sampler answers the other half, and the two compose: a sample stops at the innermost open profiling scope
// (cc::rec::current_scope_frame), so it captures only the part the scope stack does not already describe.
//
// **A sample is written to the SAMPLER's stream, not the sampled thread's.**
// A suspended thread may be mid-event or mid-rotation holding the pool lock, so writing into its stream from outside
// would corrupt it or deadlock against it.
// Instead each sample carries an ANCHOR — the sampled thread, and how far its stream had committed — which is enough
// to put the sample back exactly where it belongs, along with everything the consumer had carried to that point: the
// trace, the ambient context, and the open scopes.
// `cc::rec::splice_samples` (recording.hh) is what does the putting back.
//
// **Off by default, and it perturbs what it measures far more than instrumentation does.**
// Suspending a thread costs microseconds, so the rate is a budget across all threads rather than a per-thread rate.

namespace cc::rec
{
struct sampling_config;
struct sampling_stats;
struct sampling_scope;
} // namespace cc::rec

struct cc::rec::sampling_config
{
    /// Samples per second across ALL threads, not per thread.
    ///
    /// One thread is sampled per tick, round-robin, so a thread's own rate is this divided by the number of recording
    /// threads — which is what keeps the cost fixed as a process grows threads.
    f64 rate_hz = 1000.0;

    /// The most frames one sample keeps; deeper stacks are truncated and say so.
    isize max_frames = 64;

    /// How much to randomize each interval, as a fraction of it.
    ///
    /// A perfectly periodic sampler aliases with a periodic workload — a 60 Hz frame loop sampled at exactly 1 kHz
    /// lands on the same phase every time and reports a confident lie.
    f64 jitter = 0.15;

    /// Stop each walk at the sampled thread's innermost open profiling scope.
    ///
    /// Cheaper and much smaller, and loses nothing: the frames below are what the scope stack already names, and
    /// splicing puts them back.
    bool stop_at_scope = true;
};

namespace cc::rec
{
/// Starts sampling every recording thread, round-robin.
///
/// **Only threads the recorder already knows about**, which a thread joins by recording anything at all — a log line,
/// a scope, or the ambient delta an async worker writes on its own.
/// A thread that has never recorded is invisible here, which is the limitation to know: enumerating the process's OS
/// threads instead would cover it, and is in the TODO.
///
/// Requires an initialized recorder, and does nothing where a foreign thread's stack cannot be walked
/// (`cc::stack_capture_from_context_available`).
/// Calling it while already sampling replaces the configuration.
void start_sampling(rec::sampling_config const& cfg = {});

/// Stops the sampler and waits for it, so nothing is in flight when this returns.
void stop_sampling();

[[nodiscard]] bool is_sampling();

} // namespace cc::rec

/// How many samples were taken and how many were lost, since the last start.
struct cc::rec::sampling_stats
{
    u64 taken = 0;

    /// A target that could not be suspended, or whose walk found nothing.
    u64 failed = 0;

    /// Ticks that found no thread worth sampling.
    u64 idle = 0;
};

namespace cc::rec
{
[[nodiscard]] rec::sampling_stats sampling_statistics();
} // namespace cc::rec

/// Samples for the duration of a scope, which is the shape a benchmark or a suspicious frame wants.
struct cc::rec::sampling_scope
{
    explicit sampling_scope(rec::sampling_config const& cfg = {}) { rec::start_sampling(cfg); }
    ~sampling_scope() { rec::stop_sampling(); }

    sampling_scope(sampling_scope const&) = delete;
    sampling_scope& operator=(sampling_scope const&) = delete;
};

namespace cc::rec::impl
{
/// The layout of a sample: who was sampled, where their stream had got to, and the frames.
///
/// The anchor is a POSITION rather than a copy of the state at that position, which is what lets a consumer recover
/// the trace, the ambient context and the open scope stack — all of which it already carries while replaying.
inline constexpr rec::field sample_fields[] = {
    {.name = "thread_index", .type = rec::type_code::u32_, .offset = 0, .size = 4},
    {.name = "chunk_offset", .type = rec::type_code::u32_, .offset = 4, .size = 4},
    {.name = "chunk_seq", .type = rec::type_code::u64_, .offset = 8, .size = 8},
    {.name = "frames", .type = rec::type_code::u64_array, .offset = 16, .size = 4},
};

/// Where a sample's frames start, past the fixed part and the array's own count.
inline constexpr isize sample_frames_offset = 20;

/// The descriptor every sample is written through.
/// One site, because a sample has no source location worth naming — the frames ARE the location.
[[nodiscard]] rec::desc const& sample_desc();
} // namespace cc::rec::impl
