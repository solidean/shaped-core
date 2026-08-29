#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_completion_group.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

/// One async download handed to the readback actor.
///
/// The source is a weak ref resolved at stage time — a source whose every handle was dropped before the actor ran
/// cancels the future rather than delivering bytes that were never read.
struct sg::backend::vulkan::vulkan_async_download_job
{
    std::weak_ptr<vulkan_buffer const> buffer_source;
    isize src_offset = 0;
    isize size_in_bytes = 0;

    /// Where the bytes land, and what settles the future.
    /// `pin` is weak: a caller that dropped the future before the copy ran gets a cancellation, and the actor still
    /// signals the completion value so a later writer never hangs.
    cc::span<byte> destination;
    std::weak_ptr<void const> pin;
    cc::shared_async<cc::unit> completion;

    /// This readback's value on the source's own download timeline.
    vulkan_group_value completion_value;

    /// Defer the read until this graphics-queue token completes, so it reads what the last writer left.
    sg::submission_token wait_token = sg::submission_token::not_submitted;

    /// And until any async upload to the same buffer has landed.
    ///
    /// The graphics token says nothing about it: an upload runs on its own queue, deliberately decoupled from the
    /// epoch cycle, so nothing else orders the two.
    /// dx12 needs the same edge; it just spells it as a Wait on the upload queue's fence.
    vulkan_group_value upload_wait;
};

/// Drains readbacks in enqueue order, which is what preserves per-source ordering.
class sg::backend::vulkan::vulkan_download_async_actor final : public cc::threaded_actor_impl<vulkan_async_download_job>
{
public:
    explicit vulkan_download_async_actor(vulkan_download_async_system& system) : _system(system) {}

protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-vulkan-download-async"; }
    void on_message(vulkan_async_download_job job) override;

private:
    vulkan_download_async_system& _system;
};

/// Async GPU→CPU transfer on the transfer queue, decoupled from epochs (`ctx.download`).
///
/// The mirror of vulkan_upload_async_system, with the copy the other way round and one extra step: the bytes are
/// only in host memory once the copy has *finished*, so the actor waits for its own completion value before the
/// memcpy out and only then settles the future.
///
/// **Its own queue.** Sharing one with uploads would put a readback deferred behind a graphics submission in front of
/// an unrelated upload in the same FIFO — see vulkan_context::download_queue.
class sg::backend::vulkan::vulkan_download_async_system
{
public:
    [[nodiscard]] cc::result<cc::unit> initialize(vulkan_context& ctx, isize window_bytes);

    /// Records an async readback of `size_in_bytes` from `buffer` at `offset`.
    /// Stamps the buffer so a later graphics-queue writer waits, and hands the job to the actor.
    /// An empty range is a ready empty future.
    [[nodiscard]] sg::bytes_future download_buffer(sg::raw_buffer_handle const& buffer, isize offset, isize size_in_bytes);

    void shutdown();

    /// Runs one job on the actor thread: submits the copy, waits for it, then delivers.
    void process(vulkan_async_download_job& job);

private:
    void wait_for_window(int slot);

    vulkan_context* _ctx = nullptr;

    VkBuffer _staging = VK_NULL_HANDLE;
    VkDeviceMemory _staging_memory = VK_NULL_HANDLE;
    byte* _mapped = nullptr;
    isize _window_bytes = 0;

    VkSemaphore _window_timeline = VK_NULL_HANDLE;
    u64 _window_next_value = 0;

    static constexpr int k_window_count = 3;
    u64 _window_values[k_window_count] = {0, 0, 0};
    VkCommandPool _window_pools[k_window_count] = {};
    VkCommandBuffer _window_buffers[k_window_count] = {};
    int _next_window = 0;

    cc::unique_ptr<cc::threaded_actor<vulkan_async_download_job>> _actor;
};
