#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_completion_group.hh>
#include <shaped-graphics/fwd.hh>

/// One async upload handed to the copy actor.
///
/// `src`'s pin holds the source bytes alive until they have been staged; the job is then destroyed on the actor
/// thread, off the submission path.
/// The destination is a weak ref resolved at stage time — a destination whose every handle was dropped before the
/// actor ran skips the copy, and signals its completion value anyway, so the lifetime gate and any forward reader
/// stamped with it never hang.
struct sg::backend::vulkan::vulkan_async_upload_job
{
    std::weak_ptr<vulkan_buffer const> buffer_target;
    isize dst_offset = 0;
    cc::pinned_data<byte const> src;

    /// This upload's completion value, on the destination's own upload timeline.
    vulkan_group_value completion;

    /// Defer the copy until this graphics-queue token completes, so it never overwrites bytes an
    /// already-submitted list still reads.
    /// Captured at *enqueue*, not at stage time: a token created later could belong to a list that is itself waiting
    /// on this upload, which is the cycle the interleaved-writes test exists to catch.
    sg::submission_token wait_token = sg::submission_token::not_submitted;

    /// And until any async readback of the same buffer has finished, so a write never overtakes a read that is
    /// already in flight on the other transfer queue.
    vulkan_group_value download_wait;
};

/// Drains uploads in enqueue order, which is what preserves the per-destination ordering a completion timeline
/// assumes — see vulkan_completion_group.
class sg::backend::vulkan::vulkan_upload_actor final : public cc::threaded_actor_impl<vulkan_async_upload_job>
{
public:
    explicit vulkan_upload_actor(vulkan_upload_async_system& system) : _system(system) {}

protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-vulkan-upload-async"; }
    void on_message(vulkan_async_upload_job job) override;

private:
    vulkan_upload_async_system& _system;
};

/// Async CPU→GPU transfer on the transfer queue, decoupled from epochs.
/// Reached via `ctx.upload.bytes_to_buffer`.
///
/// A cc::threaded_actor memcpys source bytes into a persistently-mapped staging buffer and records a copy on the
/// transfer queue.
/// The staging buffer is **triple-buffered** into fixed-size windows, so CPU memcpy and GPU copy overlap, and an
/// upload larger than a window packs across successive windows.
///
/// **Where Vulkan is simpler than dx12 here.** A timeline semaphore is both a fence and its counter, and one submit
/// can wait on and signal several of them across queues — so the cross-queue handshake in both directions is two
/// entries in a submit rather than explicit Wait/Signal calls on two queues.
/// What stays identical is the policy: a per-*resource* completion timeline, because completion order matches
/// reservation order only within one destination (see vulkan_completion_group).
class sg::backend::vulkan::vulkan_upload_async_system
{
public:
    /// Persistently maps the staging buffer (three windows of `window_bytes` > 0), creates the window timeline and
    /// the per-window command pools, then starts the copy actor.
    [[nodiscard]] cc::result<cc::unit> initialize(vulkan_context& ctx, isize window_bytes);

    /// Records an async upload of `data` into `buffer` at `offset`.
    /// Reserves a completion value, stamps the buffer so later graphics-queue readers wait on it, and hands the job
    /// to the actor.
    /// Empty data is a no-op.
    void upload_buffer(sg::raw_buffer_handle const& buffer, cc::pinned_data<byte const> data, isize offset);

    /// Requests a new staging window size in bytes (> 0), applied by the actor between windows.
    void set_window_bytes(isize bytes);

    /// Drains queued copies, waits for the transfer queue to idle, then releases everything.
    void shutdown();

    [[nodiscard]] isize window_bytes() const { return _window_bytes; }

    /// Runs one job on the actor thread: stages its bytes and submits the copies.
    void process(vulkan_async_upload_job& job);

private:
    /// Waits until window `slot` is free, i.e. the copy that last used it has finished.
    void wait_for_window(int slot);

    /// Rebuilds staging at `_desired_window_bytes` when it differs; drains first.
    void apply_pending_window_bytes();

    [[nodiscard]] cc::result<cc::unit> build_staging(isize window_bytes);
    void release_staging();

    vulkan_context* _ctx = nullptr;

    VkBuffer _staging = VK_NULL_HANDLE;
    VkDeviceMemory _staging_memory = VK_NULL_HANDLE;
    byte* _mapped = nullptr;
    isize _window_bytes = 0;

    /// Per-window reuse timeline: a window is free once this reaches the value its last copy signalled.
    VkSemaphore _window_timeline = VK_NULL_HANDLE;
    u64 _window_next_value = 0;

    static constexpr int k_window_count = 3;
    u64 _window_values[k_window_count] = {0, 0, 0};
    VkCommandPool _window_pools[k_window_count] = {};
    VkCommandBuffer _window_buffers[k_window_count] = {};
    int _next_window = 0;

    /// A pending set_window_bytes request; the actor compares it to _window_bytes and rebuilds staging when they
    /// differ.
    cc::atomic<isize> _desired_window_bytes = {0};

    cc::unique_ptr<cc::threaded_actor<vulkan_async_upload_job>> _actor;

    friend class vulkan_upload_actor;
};
