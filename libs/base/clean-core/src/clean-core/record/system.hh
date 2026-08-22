#pragma once

#include <clean-core/record/fwd.hh>
#include <clean-core/record/listener.hh>

// Bringing the recording system up, feeding listeners, and getting everything out on demand.
//
// **Nothing is recorded before cc::rec::initialize().**
// That is deliberate: a library that records must not decide on behalf of the program how many megabytes it may
// have, and an application that never calls initialize pays only the disabled-site gate.
// Test and example binaries get it from nx::run, so neither needs a line of setup.

/// What the recording system is allowed to cost, and how it behaves when it runs out.
struct cc::rec::config
{
    /// Bytes per chunk, header included.
    /// Large on purpose: acquiring one is then rare enough for a plain mutex, and the per-chunk consumer costs amortize.
    isize chunk_bytes = 1 << 20;

    /// The total the pool may ever allocate, across every thread.
    /// A per-thread budget would be the obvious knob and the wrong one — fifty threads times a few megabytes is a
    /// gigabyte nobody asked for.
    isize budget_bytes = 64 << 20;

    rec::overflow_policy overflow = rec::overflow_policy::drop;

    /// Whether events carry the core they were recorded on.
    /// Worth about ten cycles an event, and the usual explanation for a step in otherwise steady timings.
    bool capture_core_id = true;

    /// Whether a background thread drains into the listeners.
    /// Without one, draining happens on whichever thread flushes — which is what a deterministic test wants.
    bool threaded = true;

    /// How often the background thread looks for new events.
    f64 poll_interval_secs = 0.001;

    /// How long the write path waits before asking an exhausted pool again.
    /// Without a floor here a drop storm would take the pool lock once per dropped event.
    f64 drop_retry_secs = 0.001;

    /// How many pre-faulted chunks the pool keeps ready, so a producer never takes a page fault.
    isize ready_chunks = 4;

    /// The defaults for this build.
    ///
    /// Release drops rather than blocking, because recording must never change the timing of what it measures.
    /// Assertion-enabled builds apply backpressure instead, so nothing goes missing while you are looking for it.
    [[nodiscard]] static config create_default();
};

/// A snapshot of how the recording system itself is doing.
struct cc::rec::system_stats
{
    isize threads = 0;
    isize registered_listeners = 0;
    isize allocated_bytes = 0;
    isize ready_chunks = 0;

    /// Acquisitions that came back empty-handed — the pool's own health metric.
    u64 failed_acquires = 0;

    u64 chunks_processed = 0;
    u64 events_processed = 0;
};

namespace cc::rec
{
/// Brings the system up.
/// Idempotent in the sense that a second call asserts rather than reconfiguring.
void initialize(rec::config const& cfg = rec::config::create_default());

/// Drains everything outstanding, stops the background thread, and releases the pool.
///
/// **No other thread may be recording**, and every listener must already be unregistered.
/// Shutdown invalidates every registered thread's write cursor, which is safe against a quiescent thread and not
/// against one mid-event.
void shutdown();

[[nodiscard]] bool is_initialized();
[[nodiscard]] rec::config const& current_config();

/// The measured rate of the cycle counter, in cycles per second, or 0 where there is no counter.
///
/// Calibrated once at initialize(). Only a LIVE chunk needs it — a sealed one carries both ends of its own span and
/// maps cycles to wall time exactly.
[[nodiscard]] f64 cycles_per_second();

// register_listener / unregister_listener are declared in listener.hh, next to the handle they hand out.

/// Drains everything committed at the moment of the call into every registered listener, on the CALLING thread.
///
/// The guarantee is a happens-before one: every event this thread — or any thread — published before the call has been
/// offered to every listener by the time it returns.
/// Events published concurrently may or may not be included.
void flush_blocking();

[[nodiscard]] rec::system_stats stats();
} // namespace cc::rec
