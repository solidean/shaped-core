#pragma once

#include <clean-core/record/fwd.hh>

// The per-thread write cursor, and the single most performance-sensitive object in the system.
//
// **Everything here is trivially destructible and constant-initialized, deliberately.**
// A thread_local with a non-trivial destructor emits a one-time-initialization guard check on EVERY access under
// MSVC and clang-cl, which would roughly double the cost of a recording site.
// Thread registration and the exit handshake hang off a separate sentinel that writer.cc installs from the cold path,
// so the hot path never sees it.
//
// A fresh thread starts with cur == end == nullptr, so its first record fails the bounds check and lands in the cold
// path naturally.
// That is where registration happens, and it costs the call site nothing.

/// One thread's write cursor.
struct cc::rec::impl::writer_tls
{
    byte* cur;
    byte* end;
    rec::chunk* current;

    /// The cc::async ambient chain head last written into this thread's stream.
    /// Compared against the live one to turn ambient context into a state delta rather than a per-event field.
    void* last_ambient;

    /// How many profiling scopes are open on this thread.
    u32 scope_depth;

    /// The listener layer this thread is currently recording under, plus one; 0 means ordinary code.
    /// See libs/base/clean-core/docs/systems/recording.md on re-entrancy.
    u16 layer_plus_one;

    /// How deep inside listener dispatch this thread is.
    /// **At depth 2 and beyond nothing is recorded**: a listener may record, but a listener reached BY another
    /// listener's recording may not, which is what makes the layer rule terminate without a cycle detector.
    u16 layer_depth;

    rec::impl::thread_state* state;

    /// The cursor set aside while this thread is inside listener dispatch, so listener output lands in its own chunks.
    ///
    /// A chunk carries one layer, so these are swapped in and out rather than shared with ordinary recording.
    /// **They come with a thread_state of their own**, deliberately: a chunk queue is consumed strictly in order, so a
    /// half-full listener chunk sitting in the ordinary queue would stall everything written after it.
    byte* alt_cur;
    byte* alt_end;
    rec::chunk* alt_current;
    rec::impl::thread_state* alt_state;

    /// Which incarnation of the recording system this cursor belongs to.
    ///
    /// initialize() and shutdown() each bump the global counter, and the cold path resets the whole cursor when it
    /// finds a mismatch.
    /// Without it a thread that recorded before a shutdown would come back holding pointers into a pool that no longer
    /// exists — which no amount of care at shutdown could reach, since the state is thread-local.
    u64 generation;

    /// Cycle count before which the cold path will not ask the pool again.
    /// Without it a drop storm would take the pool lock once per dropped event, which is exactly the wrong shape.
    u64 retry_at_cycles;
};

namespace cc::rec::impl
{
inline thread_local writer_tls t_writer = {};
} // namespace cc::rec::impl
