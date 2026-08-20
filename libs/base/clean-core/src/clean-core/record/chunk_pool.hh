#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/thread/mutex.hh>

// The chunk pool: one global byte budget, handed out in whole chunks.
//
// Per-thread buffers would be the obvious design and are the wrong one — fifty threads times a few megabytes is a
// gigabyte nobody asked for, and the knob the policies actually want to express is a total.
// Chunks are large (a megabyte by default) precisely so that acquiring one is rare enough for a plain mutex, and so
// that the per-chunk costs the consumer pays — the state preamble above all — amortize away.

/// A fixed byte budget, handed out as recycled chunks.
///
/// A chunk is one allocation: the header, then its data, so a crash dump can write the whole thing without a gather.
/// Recycled chunks are already faulted in, and `refill` pre-faults fresh ones on the actor, so a producer taking a
/// page fault is a startup-only event.
struct cc::rec::chunk_pool
{
    /// `chunk_bytes` is the total per chunk including its header; `budget_bytes` caps what the pool ever allocates.
    /// A `grow_unbounded` policy ignores the budget, which is what a test run wants.
    ///
    /// **The budget must hold at least two chunks per recording thread.**
    /// A thread's queue is consumed in order and the consumer can only release a chunk once the NEXT one exists, so one
    /// chunk per thread is always retained; a budget of exactly one would never free anything again.
    chunk_pool(isize chunk_bytes, isize budget_bytes, rec::overflow_policy policy);
    ~chunk_pool();

    chunk_pool(chunk_pool const&) = delete;
    chunk_pool& operator=(chunk_pool const&) = delete;

    /// Hands `owner` a chunk with one reference, or null when the budget is exhausted under `drop`.
    /// Under `backpressure` this blocks until one frees up; under `grow_unbounded` it always succeeds.
    [[nodiscard]] rec::chunk* acquire(rec::impl::thread_state* owner, u64 seq, u16 layer);

    /// Takes a chunk back once nothing references it.
    /// Called by chunk::release_ref, not directly.
    void recycle(rec::chunk* c);

    /// Tops the ready list up to `target` chunks, touching every page.
    /// Runs on the actor.
    void refill(isize target);

    // introspection
public:
    [[nodiscard]] isize chunk_bytes() const { return _chunk_bytes; }
    [[nodiscard]] isize data_bytes_per_chunk() const;
    [[nodiscard]] isize budget_bytes() const { return _budget_bytes; }
    [[nodiscard]] isize allocated_bytes() const;
    [[nodiscard]] isize ready_count() const;

    /// How many acquisitions came back empty-handed, which is the cold path's own health metric.
    [[nodiscard]] u64 failed_acquires() const;

private:
    struct state
    {
        cc::vector<rec::chunk*> ready;
        cc::vector<cc::allocation<byte>> blocks;
        isize allocated_bytes = 0;
        u64 failed_acquires = 0;
    };

    /// Allocates one more chunk if the budget allows; returns null otherwise.
    /// Call with `_state` held.
    rec::chunk* _grow_locked(state& s);

    isize _chunk_bytes = 0;
    isize _budget_bytes = 0;
    rec::overflow_policy _policy = rec::overflow_policy::drop;
    cc::mutex<state> _state;
};
