#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

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
    };

    /// Reserves contiguous ring space for the current epoch at a multiple of `alignment_in_bytes`, blocking on an
    /// in-flight epoch when full.
    /// Image copies need the alignment; buffer copies do not.
    /// See the upload ring's reserve for why.
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
    };

    /// Frees the leading run of checkpoints that are both retired and fully drained.
    void reclaim(ring_state& s, sg::epoch completed);

    vulkan_context* _ctx = nullptr;
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    byte const* _mapped = nullptr;
    isize _capacity = 0;
    sg::epoch _last_completed = sg::epoch::first;
    cc::mutex<ring_state> _state;
    cc::unique_ptr<cc::threaded_actor<vulkan_download_copy_job>> _actor;
};
