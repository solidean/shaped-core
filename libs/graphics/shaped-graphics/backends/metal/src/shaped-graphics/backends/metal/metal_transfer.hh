#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

/// The off-frame transfer path: a second MTL4 queue, its own timeline, and the ordering that makes a command list and
/// an async copy of the same resource compose.
///
/// **A second queue is the whole point.**
/// An async transfer exists so a large upload does not sit in the frame's queue behind the frame, and sg's contract is
/// that it never blocks the caller.
/// Metal makes the queue itself cheap — `newMTL4CommandQueue` again — so what costs something is the ordering, not the
/// queue.
///
/// **Ordering runs on one shared event, in both directions.**
/// A transfer signals `_timeline` when it completes, and a command list that touches the resource waits on the value
/// its last transfer claimed.
/// The reverse — a transfer waiting for the frame's writes — rides the submission timeline the epoch system already
/// keeps, which is why there is only one new event here rather than two.
///
/// Every transfer's bytes are staged in a buffer of its own rather than a ring: a ring's reclamation is tied to the
/// epoch cycle, and an off-frame transfer is precisely the thing that does not follow it.
class sg::backend::metal::metal_transfer_system
{
public:
    /// Creates the queue and the timeline.
    /// Called once, before any transfer.
    void create(metal_context& ctx);

    /// Copy `data` into `buffer` at `offset_in_bytes`, off the frame path.
    /// The pin is consumed on the calling thread, so the caller's bytes are free the moment this returns.
    void upload_to_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> const& data, isize offset_in_bytes);

    /// Read `size_in_bytes` from `buffer` back to the host, off the frame path.
    /// Dropping the returned future cancels the copy.
    [[nodiscard]] sg::bytes_future download_from_buffer(sg::raw_buffer_handle buffer,
                                                        isize offset_in_bytes,
                                                        isize size_in_bytes);

    /// The timeline value a command list must wait for before it may touch `buffer`, or 0 for none.
    [[nodiscard]] u64 pending_value_for(sg::raw_buffer const& buffer) const;

    /// The event a wait is expressed on; the direct queue waits on it at submit.
    [[nodiscard]] MTL::SharedEvent* timeline() const { return _timeline; }

    /// Whether any transfer is still outstanding — what `block_until_transfers_drained` waits on.
    [[nodiscard]] bool has_pending() const { return _pending.load(std::memory_order_acquire) > 0; }

    /// Drains and releases the queue and the timeline.
    void shutdown();

private:
    /// A transfer's own timeline value, plus the one the same resource's previous transfer claimed (0 for none).
    struct claimed
    {
        u64 value = 0;
        u64 previous = 0;
    };

    /// Claims the next value for `buffer` and hands back the one it replaces.
    /// Both under one lock, because the previous value is exactly what the new transfer has to wait for.
    [[nodiscard]] claimed claim_value(sg::raw_buffer const& buffer);

    /// Drops `buffer`'s entry once `value` has completed, unless a newer transfer has since claimed it.
    void forget_value(sg::raw_buffer const& buffer, u64 value);

    /// Orders the transfer queue behind the last direct-queue submission that named `buffer`, and behind that buffer's
    /// own previous transfer.
    /// Called with the command buffer open, since an MTL4 queue wait is queue-sequential like the commit it precedes.
    void wait_for_queues(metal_buffer const& buffer, u64 previous_transfer);

    /// The next timeline value, claimed under `_state` so a value and its record are published together.
    struct state
    {
        u64 next_value = 1;

        /// Per resource, the highest transfer value claimed against it.
        /// Keyed on the resource's address, which is safe because an entry is erased when its resource's transfers
        /// have all completed — the recycled-address hazard needs a *cache* that outlives the resource, and this does
        /// not.
        cc::map<sg::raw_buffer const*, u64> pending_by_buffer;
    };

    metal_context* _ctx = nullptr;
    MTL4::CommandQueue* _queue = nullptr;
    MTL::SharedEvent* _timeline = nullptr;

    // A callback mutex, because `forget_value` runs inside a commit handler — see metal_common.hh.
    mutable callback_mutex<state> _state;
    std::atomic<int> _pending = 0;
};
