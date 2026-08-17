#pragma once

#include <blob-cache/fwd.hh>
#include <blob-cache/keys.hh>
#include <clean-core/container/map.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/mutex.hh>

/// In-process singleflight: at most one live acquire pipeline per logical key.
///
/// The cover is the WHOLE pipeline — lookup, compute, store — and not merely the compute.
/// Covering only the compute would let two callers both look up, both miss, and both compute, which is the exact duplication this exists to prevent.

namespace bcache::impl
{
/// One slot: the operation, held WEAKLY, and the generation that tells it apart from its successors.
///
/// Weak on purpose.
/// An owning table would keep every blob it ever handed out alive, turning a disk cache into an unbounded in-memory
/// one — so the operation lives exactly as long as somebody is still waiting on it, and the slot then expires.
/// A dead slot is not swept by anything; the next claim overwrites it in place.
struct flight_slot
{
    cc::weak_async<blob> operation;
    u64 generation = 0;
};

/// The in-process singleflight table.
///
/// **The lock covers one map probe and one insert, and nothing else.** No database call, no mailbox enqueue and no compute ever runs under it — which is also why an acquire may be
/// issued from a thread that already holds a caller's own lock.
class flight_table
{
public:
    /// What a caller got: the shared operation, and whether it is the one that must build the pipeline.
    struct claim
    {
        cc::shared_async<blob> operation;

        /// False means JOIN: `operation` belongs to somebody else, and this caller must not compute.
        bool is_owner = false;
    };

    /// A generation nobody else will use, for an operation about to be claimed.
    ///
    /// Handed out BEFORE the claim, because the pipeline has to know which slot it will eventually release and the
    /// frame is built before the probe — building it under the lock is exactly what this design forbids.
    [[nodiscard]] static u64 next_generation();

    /// Joins the live operation for `key`, or registers `fresh` under `generation` as the new one.
    ///
    /// `fresh` is created cold and is DISCARDED on a join, which costs one unused node and keeps the probe atomic.
    [[nodiscard]] claim claim_or_join(cache_key const& key, cc::shared_async<blob> fresh, u64 generation);

    /// Drops the slot for `key` if `generation` is still what is registered there.
    ///
    /// The generation is what makes this safe: an operation finishing after a successor already claimed the key must
    /// not erase the successor's registration, or the next caller would start a second compute for the same key.
    void release(cache_key const& key, u64 generation);

    /// Resolves every live operation as cancelled and empties the table.
    /// Called once by close(): a joiner waiting on a compute the shutdown abandoned would otherwise wait forever.
    void cancel_all();

    /// How many registered operations are still alive.
    /// Diagnostics and tests.
    [[nodiscard]] isize in_flight_count() const;

private:
    struct state
    {
        cc::map<cache_key, flight_slot> in_flight;
    };
    mutable cc::mutex<state> _state;
};
} // namespace bcache::impl
