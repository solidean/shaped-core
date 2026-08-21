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
// to put the sample back exactly where it belongs, so a reader replaying that thread's stream in order reaches it with
// the trace and the open scopes already in hand.
// `cc::rec::recording::spliced_samples` (recording.hh) is what does the putting back.
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
    /// Ticks per second, and — with the default `threads_per_tick` — each thread's own sampling rate.
    ///
    /// **Capped by what the OS timer can deliver**, which measured about 1.9 kHz on Windows: a high-resolution
    /// waitable timer floors near half a millisecond, and neither timeBeginPeriod nor a periodic timer beats it.
    /// Asking for more than that quietly gets you the floor.
    f64 rate_hz = 1000.0;

    /// How many threads one tick samples, or 0 for all of them.
    ///
    /// All of them by default, so `rate_hz` means what a profiler user expects — each thread's rate — rather than a
    /// budget divided by however many threads happen to exist.
    /// A 16 ms frame at 1 kHz is sixteen samples.
    /// Splitting those across eight threads is two per thread, which is not a profile of anything.
    ///
    /// The cost follows directly: a tick suspends and walks each thread it covers, so this multiplies the sampler's
    /// own load.
    /// Set it to 1 for a fixed budget instead.
    isize threads_per_tick = 0;

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

    /// Sample every OS thread in the process, not only the ones the recorder knows.
    ///
    /// A thread joins the recorder's set by recording something, so a thread that records nothing is invisible to it —
    /// and that is exactly the thread a profiler is looking for.
    /// Such a thread has no stream, so its samples carry frames and a native id but no anchor, and splicing leaves
    /// them where they are.
    ///
    /// **Off by default, because it costs the profile and not just the CPU.**
    /// A tick walks every thread rather than one, and measured over half a second at 1 kHz that turned 374 ticks into
    /// 220 — so the threads you asked about get sampled 40% less often, on top of the sampler going from 1.4% of a
    /// core to 7.4%.
    /// Turn it on when you are hunting a thread that records nothing, which is exactly what it is for.
    bool include_unknown_threads = false;
};

namespace cc::rec
{
/// Starts sampling, round-robin over every thread the configuration covers.
///
/// By default that is the threads the recorder already knows about, which a thread joins by recording anything at all —
/// a log line, a scope, or the ambient delta an async worker writes on its own.
/// `include_unknown_threads` widens it to every OS thread in the process, at the cost documented on that field.
///
/// Requires an initialized recorder, and does nothing where a foreign thread's stack cannot be walked
/// (`cc::stack_capture_from_context_available`).
/// Calling it while already sampling replaces the configuration.
void start_sampling(rec::sampling_config const& cfg = {});

/// Stops the sampler and waits for it, so nothing is in flight when this returns.
void stop_sampling();

[[nodiscard]] bool is_sampling();

/// Replaces the configuration of a RUNNING sampler, in effect from its next tick.
///
/// Everything is live, including the rate: nothing here needs the sampler stopped, so a rate can be turned up around a
/// suspicious stretch without losing the samples already taken.
/// The intended callers are a checkbox in a profiler UI and a test narrowing what it samples.
///
/// Does nothing useful before `start_sampling`, which replaces the configuration wholesale.
void reconfigure_sampling(rec::sampling_config const& cfg);

/// What the sampler is running with right now.
[[nodiscard]] rec::sampling_config current_sampling_config();

} // namespace cc::rec

/// Applies a sampling configuration for a scope, putting the previous one back afterwards.
///
/// For turning something on around the code being investigated rather than for a whole run:
///
///     cc::rec::sampling_override const all_threads({.include_unknown_threads = true});
///
/// Composes with `sampling_scope`, which starts and stops the sampler; this only changes what a running one does.
struct cc::rec::sampling_override
{
    explicit sampling_override(rec::sampling_config const& cfg) : _previous(rec::current_sampling_config())
    {
        rec::reconfigure_sampling(cfg);
    }

    ~sampling_override() { rec::reconfigure_sampling(_previous); }

    sampling_override(sampling_override const&) = delete;
    sampling_override& operator=(sampling_override const&) = delete;

private:
    rec::sampling_config _previous;
};

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
/// The anchor is a POSITION rather than a copy of the state at that position, which is what lets a consumer recover the
/// trace and the open scope stack — both of which it already carries while replaying that thread in order.
inline constexpr rec::field sample_fields[] = {
    {.name = "thread_index", .type = rec::type_code::u32_, .offset = 0, .size = 4},
    {.name = "chunk_offset", .type = rec::type_code::u32_, .offset = 4, .size = 4},
    {.name = "chunk_seq", .type = rec::type_code::u64_, .offset = 8, .size = 8},
    {.name = "native_tid", .type = rec::type_code::u64_, .offset = 16, .size = 8},
    {.name = "frames", .type = rec::type_code::u64_array, .offset = 24, .size = 4},
};

/// What `thread_index` says when the sampled thread has no stream of its own.
/// Its `native_tid` is then the only identity it has, and there is nothing to anchor into.
inline constexpr u32 sample_unknown_thread = ~u32(0);

/// Where a sample's frames start, past the fixed part and the array's own count.
inline constexpr isize sample_frames_offset = 28;

/// The descriptor every sample is written through.
/// One site, because a sample has no source location worth naming — the frames ARE the location.
[[nodiscard]] rec::desc const& sample_desc();
} // namespace cc::rec::impl
