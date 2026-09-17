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
/// A resource's storage is held for the duration by the handle the completion handler keeps, and released explicitly
/// inside it: a caller is free to drop its last handle the moment an async transfer is issued, and the copy is still
/// running.
///
/// Every transfer's bytes are staged in a buffer of its own rather than a ring: a ring's reclamation is tied to the
/// epoch cycle, and an off-frame transfer is precisely the thing that does not follow it.
class sg::backend::metal::metal_transfer_system
{
public:
    /// Creates the queue and the timeline.
    /// Called once, before any transfer.
    [[nodiscard]] cc::result<cc::unit> create(metal_context& ctx);

    /// Copy `data` into `buffer` at `offset_in_bytes`, off the frame path.
    /// The pin is consumed on the calling thread, so the caller's bytes are free the moment this returns.
    void upload_to_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> const& data, isize offset_in_bytes);

    /// Read `size_in_bytes` from `buffer` back to the host, off the frame path.
    /// Dropping the returned future cancels the copy.
    [[nodiscard]] sg::bytes_future download_from_buffer(sg::raw_buffer_handle buffer,
                                                        isize offset_in_bytes,
                                                        isize size_in_bytes);

    /// Copy `pixels` into one region of one subresource, off the frame path.
    /// `pixels` is tightly packed, the layout `staging_layout_of` describes.
    void upload_to_texture(sg::raw_texture_handle texture,
                           cc::pinned_data<byte const> const& pixels,
                           sg::subresource_index const& subresource,
                           sg::texture_region const& region);

    /// Read one region of one subresource back to the host, off the frame path.
    [[nodiscard]] sg::bytes_future download_from_texture(sg::raw_texture_handle texture,
                                                         sg::subresource_index const& subresource,
                                                         sg::texture_region const& region);

    /// The timeline value a command list must wait for before it may touch this resource, or 0 for none.
    /// One overload per kind, because a list tracks its buffers and its textures separately.
    [[nodiscard]] pending_transfers pending_value_for(sg::raw_buffer const& buffer) const;
    [[nodiscard]] pending_transfers pending_value_for(sg::raw_texture const& texture) const;

    /// Commits one streaming batch on the transfer queue, running `on_complete` when the GPU has finished it.
    /// The streaming actor records the copies; this owns the queue, so it owns the commit.
    void commit_stream_batch(MTL4::CommandBuffer* command_buffer,
                             MTL4::CommandAllocator* allocator,
                             cc::unique_function<void()> on_complete);

    /// Orders the transfer queue behind whatever the direct queue last did to this streaming job's resource.
    /// Called with the batch's command buffer open, like every other wait here.
    void order_stream_copy(metal_stream_job const& job);

    // Per-resource streaming timelines.
    //
    // **A stream needs a timeline of its own per resource, which the async tier does not.**
    // A list touching a streamed resource waits for the WHOLE transfer, including chunks the actor has not staged
    // yet — so the value has to be reserved when the transfer is admitted and signalled when it ends.
    // On one shared timeline that is unsound: transfers finish out of order, and a later one signalling its value
    // would report an earlier one complete.
    // Per resource it is sound, because sg runs two transfers of one resource in submission order.
    //
    // The event is signalled by the CPU rather than the queue, since what it reports is a job ending — which may be
    // a cancellation with no GPU work at all.

    /// Reserves this resource's next streaming value; the caller must signal it on every teardown path.
    [[nodiscard]] u64 reserve_stream_value(void const* resource);

    /// Marks `value` reached, releasing every list and deferred deletion waiting on it.
    void signal_stream_value(void const* resource, u64 value);

    /// The highest streaming value reserved against `resource`, and the event to wait on, or nulls for none.
    /// A list waits only when the value has not been signalled yet.
    struct stream_wait
    {
        MTL::SharedEvent* event = nullptr;
        u64 value = 0;
    };
    [[nodiscard]] stream_wait pending_stream_wait(void const* resource) const;

    /// The event a wait is expressed on; the direct queue waits on it at submit.
    /// Orders `queue` behind everything `pending` names, on whichever timelines carry it.
    void wait_for_pending(MTL4::CommandQueue* queue, pending_transfers const& pending) const;

    /// Whether any transfer is still outstanding — what `block_until_transfers_drained` waits on.
    [[nodiscard]] bool has_pending() const { return _pending.load(std::memory_order_acquire) > 0; }

    /// Drains and releases the queue and the timeline.
    void shutdown();

private:
    /// One resource's streaming timeline: the event a waiter waits on, and the next value to hand out.
    struct stream_timeline
    {
        MTL::SharedEvent* event = nullptr;
        u64 next_value = 1;
    };

    /// A transfer's own timeline value, plus the one the same resource's previous transfer claimed (0 for none).
    struct claimed
    {
        u64 value = 0;
        bool is_download = false;
        pending_transfers previous;
    };

    /// Claims the next value for `resource` and hands back the one it replaces.
    /// Both under one lock, because the previous value is exactly what the new transfer has to wait for.
    [[nodiscard]] claimed claim_value(void const* resource, bool is_download);

    /// Drops `resource`'s entry once `value` has completed, unless a newer transfer has since claimed it.
    void forget_value(void const* resource, u64 value, bool is_download);

    /// Orders the transfer queue behind everything already claiming this resource: the last direct-queue submission
    /// that named it, its own previous transfer, and any streaming transfer still filling it.
    /// Called with the command buffer open, since an MTL4 queue wait is queue-sequential like the commit it precedes.
    void wait_for_queues(MTL4::CommandQueue* queue,
                         void const* resource,
                         submission_stamp const& stamp,
                         claimed const& claim);

    /// Commits `command_buffer`, signals `value`, and releases everything the transfer owns once it has run.
    /// `on_complete` runs first, inside the same handler, and is where a download copies its bytes out.
    void commit(MTL4::CommandQueue* queue,
                MTL4::CommandBuffer* command_buffer,
                MTL4::CommandAllocator* allocator,
                MTL::Buffer* staging,
                void const* resource,
                claimed const& claim,
                cc::unique_function<void()> on_complete);

    /// The next timeline value, claimed under `_state` so a value and its record are published together.
    struct state
    {
        /// One counter per queue, because one shared event cannot take signals from two queues: they complete
        /// independently, so a later value can land first and drive the event backwards.
        u64 next_upload_value = 1;
        u64 next_download_value = 1;

        /// Per resource, the highest transfer value claimed against it.
        /// Keyed on the resource's address, which is safe because an entry is erased when its resource's transfers
        /// have all completed — the recycled-address hazard needs a *cache* that outlives the resource, and this does
        /// not.
        /// One map for buffers and textures alike: the addresses cannot collide, and the question is the same one.
        cc::map<void const*, pending_transfers> pending_by_resource;

        /// The streaming timeline of every resource that has ever been streamed, and its next value.
        ///
        /// Kept until shutdown rather than dropped with the last transfer, so a command list that has already
        /// recorded a wait on one never finds it gone.
        /// A recycled address is safe here where it is not in a cache: the value only grows, and every reserved value
        /// is signalled on every teardown path — so the worst a reuse can do is wait for something already done.
        cc::map<void const*, stream_timeline> stream_timelines;
    };

    metal_context* _ctx = nullptr;
    /// Uploads.
    MTL4::CommandQueue* _queue = nullptr;

    /// **Downloads go on a queue of their own, which is the shape vulkan already has.**
    ///
    /// Sharing one with uploads reproduced a race in the tier-1 transfer fuzz about one run in four: an async
    /// download came back all zeroes while the buffer itself was correct.
    /// Splitting them took that to zero in sixty-five runs.
    /// dx12 needs no such split — a D3D12 copy queue runs its command lists serially, where an MTL4 queue is
    /// concurrent by default.
    MTL4::CommandQueue* _download_queue = nullptr;

    /// Streaming batches go on a queue of their own, and that is a correctness requirement rather than tuning.
    ///
    /// A wait blocks everything committed after it on that queue.
    /// An async transfer ordering behind an in-flight stream therefore blocks the stream's own copies too, if they
    /// share a queue — the stream can never finish, so the wait never clears.
    /// A third queue costs one object and removes the cycle entirely.
    MTL4::CommandQueue* _stream_queue = nullptr;

    /// One per transfer queue, because a shared event cannot take signals from two: they complete independently, so a
    /// later value can land first and drive the event backwards.
    MTL::SharedEvent* _upload_timeline = nullptr;
    MTL::SharedEvent* _download_timeline = nullptr;

    // A callback mutex, because `forget_value` runs inside a commit handler — see metal_common.hh.
    mutable callback_mutex<state> _state;

    /// Transfers committed but not yet finished.
    ///
    /// `std::atomic` rather than `cc::atomic`, for the reason `callback_mutex` exists: a commit handler decrements
    /// this from a dispatch queue Apple owns, and `cc::atomic` is a plain value once `SC_THREADS` is off.

    /// Serializes claim, record, commit and signal on the transfer queue, so a claimed value is signalled in the order
    /// it was claimed in.
    ///
    /// The direct queue gets this by holding one lock across submit; here the claim happens at the top of each path and
    /// the commit well after the copy is recorded, so there is no existing section to widen.
    /// Without it two concurrent transfers can interleave between taking a value and signalling it, which moves the
    /// timeline backwards exactly as an out-of-order submit would.
    /// The CPU copy into staging stays outside it — it runs before the claim in every path, and it is the only
    /// expensive step, so uploads serialize their recording but not their copying.
    cc::mutex<int> _submit;

    std::atomic<int> _pending = 0;
};
