#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/texture_region.hh>

#include <atomic>

namespace sg::backend::dx12
{
/// One async upload handed to the copy actor. `src`'s pin holds the source bytes alive until they have
/// been staged (the job is then destroyed on the actor thread, off the submission path). `target` is a
/// weak ref resolved at stage time: if every handle to the destination was dropped before the actor ran,
/// the copy is skipped — but its `copy_fence_value` is still signaled so the lifetime gate + any forward
/// readers stamped with it never hang. `copy_fence_value` is reserved synchronously at enqueue; the
/// completion fence reaches it once the copy has run (or the job was dropped).
struct dx12_async_upload_job
{
    // Exactly one destination is set: a buffer (via `target` + `dst_offset`) or a texture (via
    // `texture_target` + `footprint`). Both are weak refs, locked at stage time — a dropped destination
    // skips the copy but still signals its completion value (see stage_job).
    std::weak_ptr<dx12_buffer const> target;          // destination buffer, or empty for a texture copy
    std::weak_ptr<dx12_texture const> texture_target; // destination texture, or empty for a buffer copy
    dx12_texture_footprint footprint;                 // the texture region's copy footprint (texture copies)
    bool is_texture = false;                          // discriminant: texture copy vs buffer copy
    cc::isize dst_offset = 0;                         // destination byte offset (buffer copies)
    cc::pinned_data<cc::byte const> src;              // source bytes + their pin
    dx12_copy_fence_value copy_fence_value = dx12_copy_fence_value::none; // completion value for this upload
    sg::submission_token wait_token
        = sg::submission_token::invalid; // defer the copy until this direct-queue token completes
};

/// Async CPU→GPU buffer streaming on a dedicated COPY queue, decoupled from epochs. Reached via
/// `ctx.upload.bytes_to_buffer`. A cc::threaded_actor memcpys source bytes into a persistently-mapped
/// UPLOAD staging buffer and records CopyBufferRegion on the copy queue.
///
/// The staging buffer is **triple-buffered** into fixed-size windows: while the GPU copies one window
/// the actor fills the next, with a third as slack — so CPU memcpy and GPU copy overlap and there is no
/// sync bubble. A window is submitted as soon as it fills (or the inbox drains), keeping latency low;
/// an upload larger than a window is packed across successive windows. Reusing a window's memory waits
/// on a per-window **staging fence** (the actor only reuses a window three submissions later, so the
/// wait is normally already satisfied). A separate **completion fence** (this system's own
/// `_completion_fence`) is signaled with the highest finished upload value each window, and the submit
/// path makes a later direct-queue list wait on it so it observes the copy (see dx12_command_list). The
/// system owns one
/// copy command list (reused across windows) plus one allocator per window slot, cycled on the window
/// fence — not the epoch-gated command pool, since the copy queue is decoupled from epochs. See
/// libs/graphics/shaped-graphics/docs/concepts/upload.async.md.
class dx12_upload_async_system
{
public:
    explicit dx12_upload_async_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates its own COPY queue, persistently maps the UPLOAD staging buffer (three windows of
    /// `window_bytes` > 0), the staging fence, the completion fence, and the actor's wait event, then starts
    /// the copy actor. Called once during context bring-up. Returns a dx12 error on failure.
    [[nodiscard]] cc::result<cc::unit> initialize(cc::isize window_bytes);

    /// Records an async upload of `data` into `buffer` at `offset`. Reserves a completion value, stamps
    /// the buffer so later direct-queue readers wait on it, and hands the job to the actor. Empty data is
    /// a no-op. Preconditions: buffer non-null, a dx12 buffer, not expired, copy_dst usage, in bounds.
    void upload_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<cc::byte const> data, cc::isize offset);

    /// Records an async upload of `data` (tightly packed) into one region of `texture`. Reserves a
    /// completion value + stamps the texture (later direct-queue readers wait on it), and hands the job to
    /// the copy actor. The texture must be in the COMMON layout on the copy queue (freshly created, or not
    /// left in a shader/attachment layout by a prior direct-queue op); a large region packs across staging
    /// windows row/slice-wise. Preconditions: non-null, a dx12 texture, not expired, copy_dst usage, data == region size.
    void upload_texture(sg::raw_texture_handle texture,
                        cc::pinned_data<cc::byte const> data,
                        sg::subresource_index subresource,
                        sg::texture_region region);

    /// Requests a new staging window size in bytes (> 0), applied by the copy actor between windows: it
    /// drains every in-flight window, then rebuilds the staging buffer at `bytes * 3`. Thread-safe; the
    /// change is picked up before the next upload is staged, so in-flight uploads are unaffected.
    void set_window_bytes(cc::isize bytes);

    /// Shuts the actor down (draining queued copies and waiting for its copy queue to idle), then releases
    /// the copy queue and unmaps + releases the staging buffer.
    void shutdown();

    // Set in initialize, then touched only by the copy actor (_staging/_mapped/_window_bytes are also
    // rebuilt by the actor when a set_window_bytes is applied) — the actor reads them lock-free.
    dx12_context& _ctx;
    // This system's own dedicated COPY queue. Async upload and download each own a separate queue so their
    // windows never FIFO-block each other: a Wait on a shared queue stalls all work behind it, which with
    // interleaved upload/download dependencies deadlocks (see
    // libs/graphics/shaped-graphics/docs/concepts/download.async.md). Created in initialize.
    ComPtr<ID3D12CommandQueue> _copy_queue;
    ComPtr<ID3D12Resource> _staging;
    cc::byte* _mapped = nullptr;
    cc::isize _window_bytes = 0;
    ComPtr<ID3D12Fence> _window_fence; // per-window monotonic timeline: window reuse + one window's copy done
    // Async-upload completion fence, owned here (the copy queue that signals it is shared, but this fence
    // is upload-only). Signaled by the copy queue up to the highest finished upload value each window; a
    // later direct-queue list that reads the buffer waits on it at submit so it observes the copy. Read
    // externally by dx12_command_list (forward wait), dx12_epoch (reclaim gate), and the download system
    // (a pending upload the read must see). Created in initialize; empty until then.
    ComPtr<ID3D12Fence> _completion_fence;
    HANDLE _wait_event = nullptr; // actor-thread wait on the window fence

    // A pending set_window_bytes request; the actor compares it to _window_bytes at the top of each
    // process cycle and rebuilds staging when they differ. Written by any thread, read by the actor.
    std::atomic<cc::isize> _desired_window_bytes = 0;

private:
    // Reserved on the caller thread (fetch_add), handed out as dx12_copy_fence_value; the actor's
    // windows signal the context completion fence up to the highest finished value.
    std::atomic<cc::u64> _next_copy_value = 0;

    cc::unique_ptr<cc::threaded_actor<dx12_async_upload_job>> _actor;
};
} // namespace sg::backend::dx12
