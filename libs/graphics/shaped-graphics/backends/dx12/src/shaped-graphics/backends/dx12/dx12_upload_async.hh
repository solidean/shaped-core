#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

namespace sg::backend::dx12
{
/// One async upload handed to the copy actor. `src`'s pin holds the source bytes alive until they have
/// been staged (the job is then destroyed on the actor thread, off the submission path). `keep_alive`
/// holds the destination buffer alive until its copy is recorded. `copy_fence_value` is reserved
/// synchronously at enqueue; the completion fence reaches it once the copy has run on the GPU.
struct dx12_async_upload_job
{
    sg::buffer_handle keep_alive;           // holds the destination buffer alive
    ID3D12Resource* dst_resource = nullptr; // destination GPU resource (owned by keep_alive)
    cc::isize dst_offset = 0;
    cc::pinned_data<cc::byte const> src;                                  // source bytes + their pin
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
/// wait is normally already satisfied). A separate **completion fence** (the context's `_copy_fence`)
/// is signaled with the highest finished upload value each window, and the submit path makes a later
/// direct-queue list wait on it so it observes the copy (see dx12_command_list). The system owns one
/// copy command list (reused across windows) plus one allocator per window slot, cycled on the window
/// fence — not the epoch-gated command pool, since the copy queue is decoupled from epochs. See
/// libs/graphics/shaped-graphics/docs/concepts/upload.async.md.
class dx12_upload_async_system
{
public:
    explicit dx12_upload_async_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates + persistently maps the UPLOAD staging buffer (three windows of `window_bytes` > 0), the
    /// staging fence and its wait event, and starts the copy actor. Called once during context bring-up,
    /// after the context's copy queue and completion fence exist. Returns a dx12 error on failure.
    [[nodiscard]] cc::result<cc::unit> initialize(cc::isize window_bytes);

    /// Records an async upload of `data` into `buffer` at `offset`. Reserves a completion value, stamps
    /// the buffer so later direct-queue readers wait on it, and hands the job to the actor. Empty data is
    /// a no-op. Preconditions: buffer non-null, a dx12 buffer, not expired, copy_dst usage, in bounds.
    void upload_buffer(sg::buffer_handle buffer, cc::pinned_data<cc::byte const> data, cc::isize offset);

    /// Shuts the actor down (draining queued copies and waiting for the copy queue to idle), then unmaps
    /// and releases the staging buffer. Must run while the context's copy queue + completion fence and
    /// command-allocator pool are alive.
    void shutdown();

    // Set once in initialize (before the actor starts) then immutable; read by the copy actor.
    dx12_context& _ctx;
    ComPtr<ID3D12Resource> _staging;
    cc::byte* _mapped = nullptr;
    cc::isize _window_bytes = 0;
    ComPtr<ID3D12Fence> _window_fence; // per-window monotonic timeline: window reuse + one window's copy done
    HANDLE _wait_event = nullptr;      // actor-thread wait on the window fence

private:
    // Reserved on the caller thread (fetch_add), handed out as dx12_copy_fence_value; the actor's
    // windows signal the context completion fence up to the highest finished value.
    std::atomic<cc::u64> _next_copy_value = 0;

    cc::unique_ptr<cc::threaded_actor<dx12_async_upload_job>> _actor;
};
} // namespace sg::backend::dx12
