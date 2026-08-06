#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture_region.hh>

#include <atomic>

namespace sg::backend::dx12
{
/// One async upload handed to the copy actor.
/// `src`'s pin holds the source bytes alive until they have been staged; the job is then destroyed on the actor thread, off the submission path.
/// `buffer_target` / `texture_target` is a weak ref resolved at stage time.
/// A destination whose every handle was dropped before the actor ran skips the copy.
/// Its `copy_fence_value` is signaled anyway, so the lifetime gate and any forward reader stamped with it never hang.
/// `copy_fence_value` is reserved synchronously at enqueue, and the completion fence reaches it once the copy has run or the job was dropped.
struct dx12_async_upload_job
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
class dx12_upload_async_system
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

    /// Requests a new staging window size in bytes (> 0), applied by the copy actor between windows.
    /// It drains every in-flight window, then rebuilds the staging buffer at `bytes * 3`.
    /// Thread-safe; the change is picked up before the next upload is staged, so in-flight uploads are unaffected.
    void set_window_bytes(isize bytes);

    /// Shuts the actor down, draining queued copies and waiting for its copy queue to idle.
    /// Then releases the copy queue and unmaps + releases the staging buffer.
    void shutdown();

    /// Runs one cycle of the copy actor on the calling thread; true if there may be more work.
    /// A no-op returning false wherever the actor has its own thread — see sg::context::pump_transfers.
    bool pump_unthreaded() { return _actor != nullptr && _actor->process_messages_if_unthreaded(); }

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

private:
    // Reserved on the caller thread (fetch_add) and handed out as dx12_copy_fence_value.
    // The actor's windows signal _completion_fence up to the highest finished value.
    std::atomic<u64> _next_copy_value = 0;

    cc::unique_ptr<cc::threaded_actor<dx12_async_upload_job>> _actor;
};
} // namespace sg::backend::dx12
