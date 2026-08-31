#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_completion_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture_access.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/transfer/impl/transfer_scheduler.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-graphics/transfer/stream_source.hh>

/// One CPU→GPU transfer handed to the copy actor, in either tier.
///
/// `src`'s pin holds the source bytes alive until they have been staged; the job is then destroyed on the actor
/// thread, off the submission path.
/// The destination is a weak ref resolved at stage time — a destination whose every handle was dropped before the
/// actor ran skips the copy, and signals its completion value anyway, so the lifetime gate and any forward reader
/// stamped with it never hang.
struct sg::backend::vulkan::vulkan_async_upload_job
{
    // Exactly one destination is set.
    std::weak_ptr<vulkan_buffer const> buffer_target;
    std::weak_ptr<vulkan_texture const> texture_target;
    bool is_texture = false;
    isize dst_offset = 0;              // buffer copies
    sg::subresource_index subresource; // texture copies
    sg::texture_region region;         // texture copies
    isize row_bytes = 0;               // texture copies: bytes per row of the region, the chunk granularity


    /// The whole payload, for a resident transfer; empty for a source-driven one.
    cc::pinned_data<byte const> src;

    /// This transfer's completion value, on the destination's own upload timeline.
    vulkan_group_value completion;

    /// Defer the copy until this graphics-queue token completes.
    /// Captured at *enqueue*, not at stage time: a token created later could belong to a list that is itself waiting
    /// on this upload, which is the cycle the interleaved-writes test exists to catch.
    sg::submission_token wait_token = sg::submission_token::not_submitted;

    /// And until any async readback of the same destination has finished.
    vulkan_group_value download_wait;

    /// Set only for a source-driven transfer: the payload is produced chunk by chunk as windows open rather than
    /// handed over resident.
    std::unique_ptr<sg::stream_source> source;

    /// Set only for a STREAMING transfer; null marks the async tier.
    /// Carries the priority and cancel flag the actor reads when picking, the progress counters it advances, and the
    /// completion node it must settle exactly once — including on every cancellation path.
    std::shared_ptr<sg::impl::stream_control> stream;

    // --- actor-thread progress, untouched by the enqueueing thread ---------------------------------

    /// Bytes of `src` already staged, or — for a source-driven transfer — of the chunk in hand.
    isize staged = 0;

    /// The chunk a source handed over and its destination offset, while it is being staged across windows.
    cc::pinned_data<byte const> chunk;
    isize chunk_offset = 0;
    bool source_done = false;

    /// Submission order, for the scheduler's family rule.
    u64 sequence = 0;

    /// Ordering family — the destination resource, so same-destination transfers still compose.
    u64 family = 0;

    /// The window value of this transfer's most recent submit.
    ///
    /// A source-driven transfer cannot know a chunk is its last until the source says `done`, which is one poll
    /// *after* that chunk was submitted — so its completion value cannot ride the submit the way a resident
    /// payload's does.
    /// Waiting for this window value instead is what says "every copy this transfer queued has landed", which is the
    /// same fact by a different route.
    u64 last_window_value = 0;
};

/// A message carrying nothing: its only job is to wake the copy actor so it re-polls sources that said `not_yet`.
/// Sources produce on their own threads, and nothing else would tell the actor that a stalled one can now answer.
struct sg::backend::vulkan::vulkan_transfer_wake
{
};

/// The wake channel handed to every stream source.
///
/// Shared and separately lockable so it can outlive the transfer that installed it: a source may hand its waker to
/// an IO thread that fires long after, and shutdown nulls the target under this lock before the actor is destroyed,
/// so a late wake finds nothing rather than a dangling actor.
class sg::backend::vulkan::vulkan_upload_waker
{
public:
    explicit vulkan_upload_waker(vulkan_upload_async_system& system)
    {
        _target.lock([&](target& t) { t.system = &system; });
    }

    /// Wake the actor, if it is still there.
    /// Safe from any thread, any number of times, at any point in teardown.
    void wake();

    /// Called from shutdown, before the actor is torn down.
    void detach()
    {
        _target.lock([](target& t) { t.system = nullptr; });
    }

private:
    struct target
    {
        vulkan_upload_async_system* system = nullptr;
    };
    cc::mutex<target> _target;
};

/// Drains uploads, and keeps the ones that cannot yet make progress.
///
/// Messages are not processed to completion in arrival order: a job goes into a pending list, and `on_process` picks
/// from it through the shared transfer scheduler.
/// That is what lets a stalled source be filled around instead of blocking the queue behind it — the property the
/// async tier does not need and the streaming tier exists for.
class sg::backend::vulkan::vulkan_upload_actor final
  : public cc::threaded_actor_impl<vulkan_async_upload_job, vulkan_transfer_wake>
{
public:
    explicit vulkan_upload_actor(vulkan_upload_async_system& system) : _system(system) {}

protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-vulkan-upload-async"; }
    void on_thread_init() override;
    void on_message(vulkan_async_upload_job job) override;
    void on_message(vulkan_transfer_wake wake) override;
    bool on_process() override;

private:
    vulkan_upload_async_system& _system;
};

/// CPU→GPU transfer on the transfer queue, decoupled from epochs — both `ctx.upload` and `ctx.stream`'s upload half.
///
/// One system serves both tiers, because they share the staging windows and the queue; what differs is the guarantee.
/// An async transfer stamps the destination so later command lists wait on it, and runs first-in-first-out.
/// A streaming one deliberately does not — it stamps only the lifetime value, so a later reader waits on nothing —
/// and trades that for a priority the scheduler honours.
///
/// The staging buffer is **triple-buffered** into fixed-size windows, so CPU memcpy and GPU copy overlap, and a
/// transfer larger than a window packs across successive windows.
///
/// **Where Vulkan is simpler than dx12 here.** A timeline semaphore is both a fence and its counter, and one submit
/// can wait on and signal several of them across queues — so the cross-queue handshake in both directions is a few
/// entries in a submit rather than explicit Wait/Signal calls on two queues.
class sg::backend::vulkan::vulkan_upload_async_system
{
public:
    [[nodiscard]] cc::result<cc::unit> initialize(vulkan_context& ctx, isize window_bytes);

    /// Records an async upload of `data` into `buffer` at `offset`.
    /// Reserves a completion value, stamps the buffer so later graphics-queue readers wait on it, and hands the job
    /// to the actor.
    /// Empty data is a no-op.
    void upload_buffer(sg::raw_buffer_handle const& buffer, cc::pinned_data<byte const> data, isize offset);

    /// The streaming twin: same staging, but it stamps only the lifetime value, so a later command list waits on
    /// nothing.
    [[nodiscard]] sg::stream_upload_handle stream_buffer(sg::raw_buffer_handle const& buffer,
                                                         cc::pinned_data<byte const> data,
                                                         isize offset);

    /// A streaming upload whose payload is produced chunk by chunk as windows open.
    [[nodiscard]] sg::stream_upload_handle stream_source_buffer(sg::raw_buffer_handle const& buffer,
                                                                std::unique_ptr<sg::stream_source> source,
                                                                isize offset);

    /// The texture forms of all three.
    /// A chunk must fall on row boundaries, a row being the smallest unit a texture copy can place.
    void upload_texture(sg::raw_texture_handle const& texture,
                        cc::pinned_data<byte const> data,
                        sg::subresource_index const& subresource,
                        sg::texture_region const& region);
    [[nodiscard]] sg::stream_upload_handle stream_texture(sg::raw_texture_handle const& texture,
                                                          cc::pinned_data<byte const> data,
                                                          sg::subresource_index const& subresource,
                                                          sg::texture_region const& region);
    [[nodiscard]] sg::stream_upload_handle stream_source_texture(sg::raw_texture_handle const& texture,
                                                                 std::unique_ptr<sg::stream_source> source,
                                                                 sg::subresource_index const& subresource,
                                                                 sg::texture_region const& region);

    void set_window_bytes(isize bytes);

    /// Enqueues a bare wake so the actor re-polls sources that reported `not_yet`; reached through the waker.
    void wake_actor();

    void shutdown();

    [[nodiscard]] isize window_bytes() const { return _window_bytes; }

    // --- actor-facing ------------------------------------------------------------------------------
    // Called only from the copy actor thread, so none of it needs a lock.

    /// Takes ownership of a job the actor has just received.
    void admit(vulkan_async_upload_job job);

    /// Picks one job and fills a window with it; returns true when more work may be possible right away.
    [[nodiscard]] bool run_one_window();

    /// Settles every finished transfer whose copies have landed.
    void settle_finished();

    /// Blocks until the oldest outstanding settlement lands, then settles it; false when there is none.
    ///
    /// Reached only once nothing can be staged, which is exactly when blocking here costs nothing: a completion node
    /// nobody settles parks its dependents forever, so sleeping with one outstanding is the one thing the actor must
    /// not do.
    [[nodiscard]] bool wait_and_settle();

    /// Whether anything is still pending or awaiting settlement.
    [[nodiscard]] bool has_work() const { return !_pending.empty() || !_awaiting.empty(); }

    /// Runs the driver's per-thread initialization now, under a leak annotation.
    ///
    /// A Vulkan ICD allocates a small per-thread block on its first call from a thread and never frees it, which
    /// LeakSanitizer reports against whichever call happened to be first.
    /// Doing it here scopes the annotation to one foreign call instead of leaving it to land on a copy — and keeps
    /// the actor thread otherwise fully instrumented, so a real leak on it is still caught.
    void warm_up_driver_thread();

private:
    /// One finished transfer waiting for its last copy to land before its completion node is settled.
    struct awaiting_settle
    {
        /// On the window timeline: reaching it means every copy this transfer queued has landed, which is when its
        /// stream control may be settled.
        /// The completion *value* is already queued by then — see signal_on_queue.
        u64 window_value = 0;

        std::shared_ptr<sg::impl::stream_control> stream;
        bool delivered = true;
    };

    void wait_for_window(int slot);
    void apply_pending_window_bytes();
    [[nodiscard]] cc::result<cc::unit> build_staging(isize window_bytes);
    void release_staging();

    /// Settles `job`'s stream control now, without waiting for a copy — for a cancelled or failed transfer.
    static void settle_now(vulkan_async_upload_job& job, bool delivered);

    /// Signals `value` with an empty submit on the upload queue.
    ///
    /// **Never a host signal.** A timeline rejects any signal that would move it backwards, and values are reserved
    /// in enqueue order while transfers finish in scheduling order — so a host signal is a violation waiting for the
    /// first transfer that finishes out of reservation order.
    /// An empty submit takes the queue's position instead, which is what makes the value mean "every copy I queued
    /// has landed" rather than "the CPU got here".
    ///
    /// It must be queued **immediately after that transfer's last copy**, not once the copy has landed: anything
    /// submitted in between would otherwise signal a higher value first.
    void signal_on_queue(vulkan_group_value const& value);

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

    cc::atomic<isize> _desired_window_bytes = {0};
    cc::atomic<u64> _next_sequence = {0};

    /// Jobs the actor has admitted and not yet finished, in no particular order — the scheduler decides.
    cc::vector<vulkan_async_upload_job> _pending;
    cc::vector<awaiting_settle> _awaiting;

    /// Which job fills the open window next, and how windows are shared between the two tiers.
    sg::impl::transfer_scheduler _scheduler;

    /// Handed to every stream source, and detached before the actor dies.
    std::shared_ptr<vulkan_upload_waker> _waker;

    cc::unique_ptr<cc::threaded_actor<vulkan_async_upload_job, vulkan_transfer_wake>> _actor;
};
