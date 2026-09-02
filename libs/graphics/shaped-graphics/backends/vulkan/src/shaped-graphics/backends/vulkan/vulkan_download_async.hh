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
#include <shaped-graphics/backends/vulkan/vulkan_texture_access.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/subresource.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-graphics/transfer/stream_sink.hh>

/// One async download handed to the readback actor.
///
/// **The job owns its source**, and that is a semantic guarantee rather than a convenience.
/// A caller who starts a readback and then drops every handle to the resource still gets their bytes: the future is
/// what keeps the source alive, and dropping the resource must never cancel a download somebody can still observe.
/// The destination is the caller's own memory, so there is always somewhere for the bytes to land.
///
/// Dropping the *future* is the other direction and does still cancel, since nothing can observe the result then.
/// See libs/graphics/shaped-graphics/docs/TODO.md for how far that cancellation could be pushed.
struct sg::backend::vulkan::vulkan_async_download_job
{
    std::shared_ptr<vulkan_buffer const> buffer_source;
    std::shared_ptr<vulkan_texture const> texture_source;
    bool is_texture = false;
    sg::subresource_index subresource; // texture readbacks
    sg::texture_region region;         // texture readbacks
    isize row_bytes = 0;               // texture readbacks: the chunk granularity
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

    /// Set only for a STREAMING readback; null marks the async tier.
    std::shared_ptr<sg::impl::stream_control> stream;

    /// Set only for a sink-driven readback: each chunk is handed over as it arrives and nothing accumulates.
    /// The span it receives points into the staging window, so it is valid for the call and no longer.
    sg::stream_sink sink;

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
    void on_thread_init() override;
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

    /// The streaming twin of download_buffer: it stamps only the lifetime value, so a later command list waits on
    /// nothing until promote_to_async is called.
    [[nodiscard]] sg::stream_download_handle stream_buffer(sg::raw_buffer_handle const& buffer,
                                                           isize offset,
                                                           isize size_in_bytes);

    /// The same, delivering each chunk to `sink` as it arrives rather than accumulating a resident result.
    [[nodiscard]] sg::stream_download_handle stream_to_sink_buffer(sg::raw_buffer_handle const& buffer,
                                                                   sg::stream_sink sink,
                                                                   isize offset,
                                                                   isize size_in_bytes);

    /// The texture forms.
    /// A readback's chunks are whole rows, the smallest unit a texture copy can address.
    [[nodiscard]] sg::bytes_future download_texture(sg::raw_texture_handle const& texture,
                                                    sg::subresource_index const& subresource,
                                                    sg::texture_region const& region);
    [[nodiscard]] sg::stream_download_handle stream_to_sink_texture(sg::raw_texture_handle const& texture,
                                                                    sg::stream_sink sink,
                                                                    sg::subresource_index const& subresource,
                                                                    sg::texture_region const& region);

    /// Records an async readback of `size_in_bytes` from `buffer` at `offset`.
    /// Stamps the buffer so a later graphics-queue writer waits, and hands the job to the actor.
    /// An empty range is a ready empty future.
    [[nodiscard]] sg::bytes_future download_buffer(sg::raw_buffer_handle const& buffer, isize offset, isize size_in_bytes);

    void shutdown();

    /// Runs one job on the actor thread: submits the copy, waits for it, then delivers.
    void process(vulkan_async_download_job& job);

    /// The driver's per-thread initialization, under a leak annotation — see the upload system's twin.
    void warm_up_driver_thread();

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
