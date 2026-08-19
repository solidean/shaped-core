#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture_region.hh>

#include <atomic>

/// bytes_waiter for an async download: ready once the copy actor has memcpy'd the readback bytes into the destination.
/// There is no "submitted" gate, unlike the inline path — an async download is always handed to the actor.
/// The actor drains every window before it sleeps, so a blocking wait always makes progress.
class sg::backend::dx12::dx12_async_download_waiter final : public sg::bytes_waiter
{
public:
    [[nodiscard]] bool wait() override
    {
        _is_ready.wait(false, std::memory_order_acquire); // blocks until mark_ready() stores true
        return true;
    }
};

/// One async download handed to the copy actor.
/// `buffer_source` / `texture_source` is held **strong** for the job's whole lifetime, so its storage stays alive across the copy-queue read.
/// No deferred-deletion gate is needed as a result.
/// `dst` lands the bytes and is kept alive by `pin`, the future's pin held weak here.
/// A caller that dropped the future before the actor reached this job leaves `pin` expired, and the copy is skipped.
/// Its `completion_value` is signaled anyway, so a later writer waiting on it never hangs.
/// `wait_token` defers the read behind the last direct-queue list that used the buffer, so it reads committed bytes.
struct sg::backend::dx12::dx12_async_download_job
{
    // Exactly one source is set: a buffer (`buffer_source` + `src_offset`/`size`) or a texture (`texture_source` + `footprint`).
    // Both are held strong across the read, so the storage survives it.
    std::shared_ptr<dx12_buffer const> buffer_source;   // read source buffer, or empty for a texture read
    std::shared_ptr<dx12_texture const> texture_source; // read source texture, or empty for a buffer read
    dx12_texture_footprint footprint;                   // the texture region's copy footprint (texture reads)
    bool is_texture = false;                            // discriminant: texture read vs buffer read
    isize src_offset = 0;
    isize size = 0;
    cc::span<byte> dst;                                 // destination bytes (valid while `pin` is)
    std::weak_ptr<void const> pin;                      // future's pin; expired == caller cancelled
    std::shared_ptr<dx12_async_download_waiter> waiter; // marked ready after the memcpy
    dx12_download_fence_value completion_value = dx12_download_fence_value::none; // reverse-sync value for this read
    sg::submission_token wait_token = sg::submission_token::invalid; // defer the read until this token completes
    // Forward cross-queue sync vs a pending async upload to the same buffer.
    // The read waits on the upload completion fence for this value, so it observes the upload — the two copy queues are independent.
    dx12_copy_fence_value upload_wait_value = dx12_copy_fence_value::none;
};

/// Async GPU→CPU readback on the dedicated COPY queue, decoupled from epochs.
/// Reached via `ctx.download.bytes_from_buffer` / `bytes_from_texture`.
/// A cc::threaded_actor records CopyBufferRegion from the source into a persistently-mapped READBACK staging buffer on that queue.
/// It then memcpys the staged bytes into the caller's destination and marks the future ready.
/// The staging buffer is **triple-buffered** into fixed-size windows, so GPU read and CPU memcpy overlap.
/// A window is submitted as soon as it fills or the inbox drains; a read larger than a window packs across successive windows.
/// A download completes only once its CPU memcpy has run, so each submitted window is kept in flight until it is **drained**.
/// Draining happens before a window's slot is reused, and for every remaining window when the inbox empties, so a future always becomes ready without an epoch advance.
/// Two fences: a per-window **staging fence** (`_window_fence`) gating window reuse, and this system's **completion fence** (`_completion_fence`).
/// A later direct-queue list that WRITES the buffer waits on the completion fence at submit.
/// The system owns one copy command list plus one allocator per window slot, cycled on the window fence — deliberately not the epoch-gated pool.
/// See libs/graphics/shaped-graphics/docs/concepts/download.async.md.
class sg::backend::dx12::dx12_download_async_system
{
public:
    explicit dx12_download_async_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates its own COPY queue, persistently maps the READBACK staging buffer (three windows of `window_bytes` > 0), both fences and the actor's wait event, then starts the copy actor.
    /// Called once during context bring-up.
    /// Returns a dx12 error on failure.
    [[nodiscard]] cc::result<cc::unit> initialize(isize window_bytes);

    /// Records an async readback of [offset, offset+size) from `buffer` and returns the pending future.
    /// Reserves a completion value, stamps the buffer so a later direct-queue writer waits on it, and hands the job to the actor.
    /// A zero-size read returns an already-ready, empty future.
    /// `buffer` must be a non-null, non-expired dx12 buffer with copy_src usage, and the range must be in bounds.
    [[nodiscard]] sg::bytes_future download_buffer(sg::raw_buffer_handle buffer, isize offset, isize size);

    /// Records an async readback of one region of `texture` and returns the pending future of tightly-packed bytes.
    /// The texture must be in the COMMON layout on the copy queue.
    /// A large region packs across staging windows row/slice-wise.
    /// `texture` must be a non-null, non-expired dx12 texture with copy_src usage.
    [[nodiscard]] sg::bytes_future download_texture(sg::raw_texture_handle texture,
                                                    sg::subresource_index const& subresource,
                                                    sg::texture_region const& region);

    /// Requests a new staging window size in bytes (> 0), applied by the copy actor between windows.
    /// It drains every in-flight window, then rebuilds the staging buffer at `bytes * 3`.
    /// Thread-safe; the change is picked up before the next download is staged, so in-flight downloads are unaffected.
    void set_window_bytes(isize bytes);

    /// Shuts the actor down, draining queued readbacks and waiting for its copy queue to idle.
    /// Then releases the copy queue and unmaps + releases the staging buffer.
    void shutdown();

    /// Runs one cycle of the copy actor on the calling thread; true if there may be more work.
    /// A no-op returning false wherever the actor has its own thread — see sg::context::pump.
    bool pump_unthreaded() { return _actor != nullptr && _actor->process_messages_if_unthreaded(); }

    // Set in initialize, then touched only by the copy actor, which reads them lock-free.
    // _staging / _mapped / _window_bytes are also rebuilt by the actor when a set_window_bytes is applied.
    dx12_context& _ctx;
    // This system's own dedicated COPY queue, separate from the upload system's, created in initialize.
    // Their windows must never FIFO-block each other — see libs/graphics/shaped-graphics/docs/concepts/download.async.md.
    ComPtr<ID3D12CommandQueue> _copy_queue;
    ComPtr<ID3D12Resource> _staging;
    byte* _mapped = nullptr;
    isize _window_bytes = 0;
    ComPtr<ID3D12Fence> _window_fence; // per-window monotonic timeline: window reuse + one window's read done
    // Async-download completion fence, owned here and download-only.
    // Signaled by the copy queue up to the highest finished read value each window.
    // A later direct-queue list that WRITES the buffer waits on it at submit, so it never overwrites bytes the read is still reading.
    // Read externally only by dx12_command_list (reverse wait).
    // Created in initialize; empty until then.
    ComPtr<ID3D12Fence> _completion_fence;
    HANDLE _wait_event = nullptr; // actor-thread wait on the window fence

    // A pending set_window_bytes request; the actor compares it to _window_bytes each process cycle and rebuilds staging when they differ.
    // Written by any thread, read by the actor.
    std::atomic<isize> _desired_window_bytes = 0;

private:
    // Reserved on the caller thread (fetch_add) and handed out as dx12_download_fence_value.
    // The actor's windows signal _completion_fence up to the highest finished read value.
    std::atomic<u64> _next_download_value = 0;

    cc::unique_ptr<cc::threaded_actor<dx12_async_download_job>> _actor;
};
