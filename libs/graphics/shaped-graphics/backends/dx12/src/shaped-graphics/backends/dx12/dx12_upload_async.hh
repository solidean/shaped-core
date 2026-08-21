#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/transfer/impl/transfer_scheduler.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-graphics/transfer/stream_source.hh>

#include <atomic>

/// One async upload handed to the copy actor.
/// `src`'s pin holds the source bytes alive until they have been staged; the job is then destroyed on the actor thread, off the submission path.
/// `buffer_target` / `texture_target` is a weak ref resolved at stage time.
/// A destination whose every handle was dropped before the actor ran skips the copy.
/// Its `copy_fence_value` is signaled anyway, so the lifetime gate and any forward reader stamped with it never hang.
/// `copy_fence_value` is reserved synchronously at enqueue, and the completion fence reaches it once the copy has run or the job was dropped.
struct sg::backend::dx12::dx12_async_upload_job
{
    // Exactly one destination is set: a buffer (`buffer_target` + `dst_offset`) or a texture (`texture_target` + `footprint`).
    // Both are weak refs, locked at stage time — see stage_job.
    std::weak_ptr<dx12_buffer const> buffer_target;   // destination buffer, or empty for a texture copy
    std::weak_ptr<dx12_texture const> texture_target; // destination texture, or empty for a buffer copy
    dx12_texture_footprint footprint;                 // the texture region's copy footprint (texture copies)
    bool is_texture = false;                          // discriminant: texture copy vs buffer copy
    isize dst_offset = 0;                             // destination byte offset (buffer copies)
    cc::pinned_data<byte const> src;                  // source bytes + their pin
    dx12_copy_fence_value copy_fence_value = dx12_copy_fence_value::none; // completion value for this upload
    sg::submission_token wait_token
        = sg::submission_token::invalid; // defer the copy until this direct-queue token completes

    // Set only for a source-driven streaming upload; the payload is produced chunk by chunk as windows open
    // rather than handed over resident, so `src` stays empty for these.
    std::unique_ptr<sg::stream_source> source;

    // Set only for a STREAMING upload; null marks the job as the async tier.
    // Carries the priority and cancel flag the actor reads when picking, the progress counters it advances, and the
    // completion node it must settle exactly once — including on every cancellation path.
    std::shared_ptr<sg::impl::stream_control> stream;
};

/// A message carrying nothing: its only job is to wake the copy actor so it re-polls sources that said `not_yet`.
/// Sources produce on their own threads, and nothing else would tell the actor that a stalled one can now answer.
struct sg::backend::dx12::dx12_transfer_wake
{
};

/// The wake channel handed to every stream source.
///
/// Shared and separately lockable so it can outlive the transfer that installed it: a source may hand its waker to
/// an IO thread that fires long after, and shutdown nulls the target under this lock before the actor is destroyed,
/// so a late wake finds nothing rather than a dangling actor.
class sg::backend::dx12::dx12_upload_waker
{
public:
    explicit dx12_upload_waker(dx12_upload_async_system& system)
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
        dx12_upload_async_system* system = nullptr;
    };
    cc::mutex<target> _target;
};

/// Async CPU→GPU streaming on a dedicated COPY queue, decoupled from epochs.
/// Reached via `ctx.upload.bytes_to_buffer` / `bytes_to_texture`.
/// A cc::threaded_actor memcpys source bytes into a persistently-mapped UPLOAD staging buffer and records CopyBufferRegion on that queue.
/// The staging buffer is **triple-buffered** into fixed-size windows, so CPU memcpy and GPU copy overlap.
/// A window is submitted as soon as it fills or the inbox drains; an upload larger than a window packs across successive windows.
/// Two fences: a per-window **staging fence** (`_window_fence`) gating window reuse, and this system's **completion fence** (`_completion_fence`).
/// A later direct-queue reader waits on the completion fence at submit.
/// The system owns one copy command list plus one allocator per window slot, cycled on the window fence — deliberately not the epoch-gated command pool.
/// See libs/graphics/shaped-graphics/docs/concepts/upload.async.md.
class sg::backend::dx12::dx12_upload_async_system
{
public:
    explicit dx12_upload_async_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates its own COPY queue, persistently maps the UPLOAD staging buffer (three windows of `window_bytes` > 0), both fences and the actor's wait event, then starts the copy actor.
    /// Called once during context bring-up.
    /// Returns a dx12 error on failure.
    [[nodiscard]] cc::result<cc::unit> initialize(isize window_bytes);

    /// Records an async upload of `data` into `buffer` at `offset`.
    /// Reserves a completion value, stamps the buffer so later direct-queue readers wait on it, and hands the job to the actor.
    /// Empty data is a no-op.
    /// `buffer` must be a non-null, non-expired dx12 buffer with copy_dst usage, and the range must be in bounds.
    void upload_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> data, isize offset);

    /// Records an async upload of tightly-packed `data` into one region of `texture`.
    /// Reserves a completion value, stamps the texture so later direct-queue readers wait on it, and hands the job to the copy actor.
    /// The texture must be in the COMMON layout on the copy queue — freshly created, or not left in a shader/attachment layout by a prior direct-queue op.
    /// A large region packs across staging windows row/slice-wise.
    /// `texture` must be a non-null, non-expired dx12 texture with copy_dst usage, and `data` must match the region size.
    void upload_texture(sg::raw_texture_handle texture,
                        cc::pinned_data<byte const> data,
                        sg::subresource_index const& subresource,
                        sg::texture_region const& region);

    /// Records a source-driven streaming upload into `buffer`, chunk offsets relative to `offset`.
    /// The actor polls `source` on its own thread as windows open, and passes the transfer over on `not_yet`.
    [[nodiscard]] sg::stream_upload_handle stream_source_buffer(sg::raw_buffer_handle buffer,
                                                                std::unique_ptr<sg::stream_source> source,
                                                                isize offset);

    /// Records a source-driven streaming upload into one region of `texture`.
    /// Chunk offsets are into the region's tightly-packed bytes and must be row-aligned.
    [[nodiscard]] sg::stream_upload_handle stream_source_texture(sg::raw_texture_handle texture,
                                                                 std::unique_ptr<sg::stream_source> source,
                                                                 sg::subresource_index const& subresource,
                                                                 sg::texture_region const& region);

    /// Enqueues a bare wake so the actor re-polls sources that reported `not_yet`; reached through dx12_upload_waker.
    void wake_actor();

    /// Records a streaming upload of `data` into `buffer` at `offset`, returning its control handle.
    /// Unlike upload_buffer this does NOT stamp the forward reader value, so a later command list waits on nothing.
    /// It stamps the streaming lifetime value instead, which only deferred deletion reads.
    [[nodiscard]] sg::stream_upload_handle stream_buffer(sg::raw_buffer_handle buffer,
                                                         cc::pinned_data<byte const> data,
                                                         isize offset);

    /// Records a streaming upload of tightly-packed `data` into one region of `texture`.
    /// Same tier and same stamping rules as stream_buffer.
    [[nodiscard]] sg::stream_upload_handle stream_texture(sg::raw_texture_handle texture,
                                                          cc::pinned_data<byte const> data,
                                                          sg::subresource_index const& subresource,
                                                          sg::texture_region const& region);

    /// Requests a new staging window size in bytes (> 0), applied by the copy actor between windows.
    /// It drains every in-flight window, then rebuilds the staging buffer at `bytes * 3`.
    /// Thread-safe; the change is picked up before the next upload is staged, so in-flight uploads are unaffected.
    void set_window_bytes(isize bytes);

    /// Shuts the actor down, draining queued copies and waiting for its copy queue to idle.
    /// Then releases the copy queue and unmaps + releases the staging buffer.
    void shutdown();

    /// Runs one cycle of the copy actor on the calling thread; true if there may be more work.
    // Set in initialize, then touched only by the copy actor, which reads them lock-free.
    // _staging / _mapped / _window_bytes are also rebuilt by the actor when a set_window_bytes is applied.
    dx12_context& _ctx;
    // This system's own dedicated COPY queue, created in initialize.
    // Upload and download must own separate queues: a Wait stalls everything behind it in a queue's FIFO, so interleaved upload/download dependencies on one shared queue deadlock.
    // See libs/graphics/shaped-graphics/docs/concepts/download.async.md.
    ComPtr<ID3D12CommandQueue> _copy_queue;
    ComPtr<ID3D12Resource> _staging;
    byte* _mapped = nullptr;
    isize _window_bytes = 0;
    ComPtr<ID3D12Fence> _window_fence; // per-window monotonic timeline: window reuse + one window's copy done
    // Async-upload completion fence, owned here and upload-only.
    // Signaled by the copy queue up to the highest finished upload value each window.
    // A later direct-queue list that reads the buffer waits on it at submit, so it observes the copy.
    // Read externally by dx12_command_list (forward wait), dx12_epoch (reclaim gate), and the download system.
    // Created in initialize; empty until then.
    ComPtr<ID3D12Fence> _completion_fence;
    HANDLE _wait_event = nullptr; // actor-thread wait on the window fence

    // A pending set_window_bytes request; the actor compares it to _window_bytes each process cycle and rebuilds staging when they differ.
    // Written by any thread, read by the actor.
    std::atomic<isize> _desired_window_bytes = 0;

    // Which job fills the open window next, and how windows are shared between the async and streaming flavors.
    // Actor-thread state like the window bookkeeping around it, so it needs no lock.
    // It lives here rather than in the actor only because the actor type is file-local to the .cc.
    sg::impl::transfer_scheduler _scheduler;

    // Handed to every stream source, and detached before the actor dies.
    // Public alongside the other actor-facing members: the actor installs it on each source it admits.
    std::shared_ptr<dx12_upload_waker> _waker;

private:
    // Reserved on the caller thread (fetch_add) and handed out as dx12_copy_fence_value.
    // The actor's windows signal _completion_fence up to the highest finished value.
    std::atomic<u64> _next_copy_value = 0;

    cc::unique_ptr<cc::threaded_actor<dx12_async_upload_job, dx12_transfer_wake>> _actor;
};
