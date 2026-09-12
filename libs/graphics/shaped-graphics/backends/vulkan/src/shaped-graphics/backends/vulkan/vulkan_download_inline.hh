#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/transfer/impl/transfer_drain.hh>

#include <atomic>

/// The readback ring behind `cmd.download`, and the actor that drains it.
///
/// A readback is in two halves that happen at different times.
/// The GPU copy into host-visible memory is recorded now and runs when the list is submitted; the memcpy out of that
/// memory into the caller's destination can only happen once the copy has finished, which is what the actor waits for.
///
/// Ring space is reclaimed at **epoch** granularity and only once every copy reserved in that epoch has drained.
/// Both conditions are needed: the epoch fence proves the GPU is done writing, and the outstanding count proves the
/// actor is done reading.
/// Freeing on either alone would hand the space to a new reservation while it is still live.

struct sg::backend::vulkan::vulkan_download_copy_job
{
    sg::submission_token token = sg::submission_token::not_submitted;

    /// Copies the staged bytes into the caller's destination; runs on the actor once the copy has completed.
    cc::unique_function<void()> deferred_cpu_copy;

    /// The destination's owner, held weakly.
    /// Expired means the caller dropped the future, which is a cancellation rather than a delivery: the bytes were
    /// never written anywhere the caller can see, so reporting success would be a lie.
    std::weak_ptr<void const> pin;

    cc::shared_async<cc::unit> completion;
    std::shared_ptr<sg::bytes_wait_gate> gate;

    /// The reserving epoch's outstanding-copy count, released when this job is done or discarded.
    std::shared_ptr<std::atomic<isize>> epoch_copies;

    /// Counts this job as outstanding for as long as it exists, and only once it has been SUBMITTED.
    /// A job still sitting in an unsubmitted command list is the caller's to submit, so it must not hold a drain
    /// waiter — see vulkan_download_inline_system::wait_until_idle.
    sg::impl::transfer_drain::token drain;
};

/// Drains readbacks in enqueue order, which is also ring-allocation order.
class sg::backend::vulkan::vulkan_download_actor final : public cc::threaded_actor_impl<vulkan_download_copy_job>
{
public:
    explicit vulkan_download_actor(vulkan_download_inline_system& system) : _system(system) {}

protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-vulkan-download"; }
    void on_message(vulkan_download_copy_job job) override;

private:
    vulkan_download_inline_system& _system;
};

class sg::backend::vulkan::vulkan_download_inline_system
{
public:
    /// Allocates the ring and starts the actor.
    /// `capacity_in_bytes` must be > 0.
    [[nodiscard]] cc::result<cc::unit> initialize(vulkan_context& ctx, isize capacity_in_bytes);

    /// Drains the actor, then destroys the ring.
    /// Safe to call twice, and on an uninitialized system.
    void shutdown();

    /// A reservation of `size_in_bytes`, plus the epoch counter the resulting job must release.
    struct reservation
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        isize offset = 0;
        byte const* mapped = nullptr;
        std::shared_ptr<std::atomic<isize>> epoch_copies;

        /// Non-null only for a reservation the ring could not hold, and then it OWNS the one-off staging buffer.
        ///
        /// A readback has a second liveness axis the epoch fence does not cover: the actor memcpys out of this memory
        /// on its own thread, after the GPU copy the epoch gates.
        /// So the recording site captures this in the job's deferred copy, and the buffer dies with the job.
        std::shared_ptr<void> keep_alive;
    };

    /// Reserves contiguous ring space for the current epoch at a multiple of `alignment_in_bytes`, blocking on an
    /// in-flight epoch when full.
    /// Image copies need the alignment; buffer copies do not.
    /// See the upload ring's reserve for why.
    ///
    /// Where waiting cannot help — a single readback larger than the ring, or one epoch's readbacks exceeding it with
    /// nothing in flight — it falls back to a one-off staging buffer carried on `reservation::keep_alive`.
    [[nodiscard]] reservation reserve(isize size_in_bytes, isize alignment_in_bytes = 1);

    /// Counts one job against its epoch, paired one-to-one with a job actually enqueued or discarded.
    /// Called when the job is created rather than at reservation, so a reservation that produces no job counts nothing.
    void account_pending_copy(std::shared_ptr<std::atomic<isize>> const& epoch_copies);

    /// Releases one counted job; called by the actor after it drains, and by the drop path.
    void on_copy_done(std::shared_ptr<std::atomic<isize>> const& epoch_copies);

    /// Stamps each job with `token`, opens its wait gate, and enqueues it in order.
    /// Called under the context's submission lock, so actor order matches submission order.
    void enqueue_submitted(sg::submission_token token, cc::vector<vulkan_download_copy_job>& jobs);

    /// Cancels every job of a dropped list and releases its counts.
    /// The reserved bytes are not freed here: they belong to the open epoch's span and are reclaimed with it.
    void discard_unsubmitted(cc::vector<vulkan_download_copy_job>& jobs);

    /// Whether `token`'s list has finished executing; the actor polls this rather than blocking on it.
    [[nodiscard]] bool submission_complete(sg::submission_token token) const;

    /// Blocks until `token`'s list has finished.
    void wait_for_submission(sg::submission_token token);

    /// Blocks until every SUBMITTED readback has been delivered, cancelled or dropped.
    ///
    /// This is what makes ctx.block_until_idle() a delivery guarantee and not just a GPU one: the copy the GPU
    /// finished still has to be memcpy'd into the caller's destination, and only the actor does that.
    /// Submitted-only on purpose — a download recorded into a list the caller has not submitted yet can never
    /// progress, so counting it would turn this into a hang rather than a wait.
    void wait_until_idle() { _drain.wait_until_idle(); }

    /// Records a pending ring capacity (> 0), applied at the next epoch boundary (apply_pending_budget).
    void set_budget(isize capacity);

    /// Applies a pending set_budget at an epoch boundary, and is a no-op when nothing is pending.
    /// Drains every in-flight epoch AND waits the actor out: the memcpy reads this ring on its own thread, so the
    /// epoch fence alone would not prove it is safe to free.
    void apply_pending_budget();

    void on_epoch_advance(sg::epoch closed);
    void on_epochs_completed(sg::epoch completed);

private:
    struct checkpoint
    {
        sg::epoch epoch_id;
        u64 end_pos;
        std::shared_ptr<std::atomic<isize>> outstanding;
    };

    struct ring_state
    {
        u64 next_pos = 0;
        u64 freed_pos = 0;
        std::shared_ptr<std::atomic<isize>> current_epoch_copies = std::make_shared<std::atomic<isize>>(0);
        cc::vector<checkpoint> checkpoints;
        isize pending_capacity = 0; ///< a set_budget awaiting the next epoch boundary (0 = none)
    };

    /// One host-visible mapped buffer, the shape both initialize and a resize build.
    struct ring_storage
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        byte const* mapped = nullptr;
    };

    /// Builds a mapped TRANSFER_DST buffer of `capacity` bytes, or nullopt if it could not be allocated.
    [[nodiscard]] cc::optional<ring_storage> create_ring(isize capacity);

    /// Frees the leading run of checkpoints that are both retired and fully drained.
    void reclaim(ring_state& s, sg::epoch completed);

    /// A dedicated staging buffer for one readback the ring could not hold, owned by the returned keep_alive.
    [[nodiscard]] reservation reserve_outside_ring(isize size_in_bytes);

    /// Says so once per epoch, naming what did not fit and what the budget is.
    void warn_outside_ring(isize size_in_bytes);

    vulkan_context* _ctx = nullptr;
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    byte const* _mapped = nullptr;
    isize _capacity = 0;
    sg::epoch _last_completed = sg::epoch::first;
    cc::mutex<ring_state> _state;
    sg::impl::transfer_drain _drain;

    /// The last epoch the fallback warning fired in, so a frame that overruns repeatedly says so once.
    cc::atomic<u64> _last_warned_epoch = 0;
    cc::unique_ptr<cc::threaded_actor<vulkan_download_copy_job>> _actor;
};
