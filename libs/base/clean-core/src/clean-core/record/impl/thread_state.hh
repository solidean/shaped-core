#pragma once

#include <clean-core/record/fwd.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>

// One recording thread, as the consumer sees it.
//
// The chunk queue between a producer and the actor is SPSC and needs no lock: the producer only ever appends through
// chunk::next_in_thread, and the consumer only ever walks forward.
// Everything expensive here — registration, naming, the exit handshake — happens once per thread, and thread churn is
// explicitly not a case this system optimizes for.

/// The per-thread record the actor consumes from, and the anchor a thread's chunks hang off.
///
/// Outlives the thread itself: an exiting thread seals its chunk and marks itself dead, but the state stays registered
/// until the actor has drained everything behind it.
struct cc::rec::impl::thread_state
{
    // identity, written once at registration
    cc::thread_id tid = cc::thread_id::invalid;
    u32 index = 0;
    char name[48] = {};

    // the SPSC chunk queue
public:
    /// The thread's first chunk.
    /// Written once by the producer, read by the consumer to find where to start.
    cc::atomic<rec::chunk*> queue_head = nullptr;

    /// The chunk the producer is currently filling.
    /// Producer-only.
    rec::chunk* produce_tail = nullptr;

    /// Where the consumer got to.
    /// Consumer-only, and never touched by the producer.
    rec::chunk* consume_cursor = nullptr;
    u32 consume_offset = 0;

    /// The stream state the consumer has carried forward to `consume_offset`.
    /// Copied into each chunk's state_at_start the first time the consumer touches it, which is what makes a chunk
    /// independently decodable without the producer writing a preamble.
    rec::stream_state* consumer_state = nullptr;

    // liveness
public:
    /// False once the thread has exited; the state is reclaimed only after the actor has drained past it.
    cc::atomic<bool> is_alive = true;

    /// The next sequence number this thread's chunks take.
    /// Producer-only.
    u64 next_seq = 0;

    // drop accounting, producer-only
public:
    /// Events lost since the last gap event, and the span they covered.
    /// Flushed into a gap event at the head of the next chunk the thread manages to claim.
    u64 dropped_events = 0;
    u64 dropped_bytes = 0;
    u64 drop_begin_cycles = 0;
    u64 drop_end_cycles = 0;

    // registry
public:
    /// The owning thread's write cursor.
    ///
    /// The one thing that reaches back into another thread's thread-local state, and it exists for exactly one reason:
    /// shutdown must be able to invalidate every cursor.
    /// A cursor with room left never enters the cold path, so no amount of checking there could stop a thread from
    /// writing into a pool that has already been freed.
    rec::impl::writer_tls* tls = nullptr;

    thread_state* registry_next = nullptr;
};
