#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>
#include <memory>

namespace sg::backend::dx12
{
/// bytes_waiter for an async download: ready once the copy actor has memcpy'd the readback bytes into the
/// destination. Unlike the inline path there is no "submitted" gate — an async download is always handed
/// to the actor, which drains every window before it sleeps, so a blocking wait always makes progress.
class dx12_async_download_waiter final : public sg::bytes_waiter
{
public:
    [[nodiscard]] bool wait() override
    {
        _is_ready.wait(false, std::memory_order_acquire); // blocks until mark_ready() stores true
        return true;
    }
};

/// One async download handed to the copy actor. `source` is held **strong** for the job's whole lifetime,
/// so its storage stays alive across the copy-queue read (no deferred-deletion gate needed). `dst` lands
/// the bytes; it is kept alive by `pin` (the future's pin, held weak here) — if the caller dropped the
/// future before the actor reached this job, `pin` has expired and the copy is skipped, but its
/// `completion_value` is still signaled so a later writer waiting on it never hangs. `wait_token` defers
/// the read behind the last direct-queue list that used the buffer (so it reads committed bytes).
struct dx12_async_download_job
{
    std::shared_ptr<dx12_buffer const> source; // read source; held strong across the read
    cc::isize src_offset = 0;
    cc::isize size = 0;
    cc::span<cc::byte> dst;                             // destination bytes (valid while `pin` is)
    std::weak_ptr<void const> pin;                      // future's pin; expired == caller cancelled
    std::shared_ptr<dx12_async_download_waiter> waiter; // marked ready after the memcpy
    dx12_download_fence_value completion_value = dx12_download_fence_value::none; // reverse-sync value for this read
    sg::submission_token wait_token = sg::submission_token::invalid; // defer the read until this token completes
};

/// Async GPU→CPU buffer readback on the dedicated COPY queue, decoupled from epochs. Reached via
/// `ctx.download.bytes_from_buffer`. A cc::threaded_actor records CopyBufferRegion from the source into a
/// persistently-mapped READBACK staging buffer on the copy queue, then memcpys the staged bytes into the
/// caller's destination and marks the future ready.
///
/// The staging buffer is **triple-buffered** into fixed-size windows: while the GPU reads one window the
/// actor fills the next, with a third as slack. A window is submitted as soon as it fills (or the inbox
/// drains); a read larger than a window packs across successive windows. Because a download completes only
/// once the CPU memcpy has run, each submitted window is kept in flight until it is **drained** — the
/// actor waits on that window's staging fence, memcpys its chunks into their destinations, and marks their
/// waiters ready — which it does before reusing the window's slot and for every remaining window when the
/// inbox empties (so a future always becomes ready without an epoch advance). A separate **completion
/// fence** (the context's `_download_copy_fence`) is signaled with the highest finished read value each
/// window, and the submit path makes a later direct-queue list that WRITES the buffer wait on it, so it
/// never overwrites bytes the read is still reading. The system owns one copy command list (reused across
/// windows) plus one allocator per window slot, cycled on the window fence — not the epoch-gated pool,
/// since the copy queue is decoupled from epochs. See
/// libs/graphics/shaped-graphics/docs/concepts/download.async.md.
class dx12_download_async_system
{
public:
    explicit dx12_download_async_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates + persistently maps the READBACK staging buffer (three windows of `window_bytes` > 0), the
    /// window fence and its wait event, and starts the copy actor. Called once during context bring-up,
    /// after the context's copy queue and download completion fence exist. Returns a dx12 error on failure.
    [[nodiscard]] cc::result<cc::unit> initialize(cc::isize window_bytes);

    /// Records an async readback of [offset, offset+size) from `buffer` and returns the pending future.
    /// Reserves a completion value, stamps the buffer so a later direct-queue writer waits on it, and hands
    /// the job to the actor. A zero-size read returns an already-ready, empty future. Preconditions: buffer
    /// non-null, a dx12 buffer, not expired, copy_src usage, in bounds.
    [[nodiscard]] sg::bytes_future download_buffer(sg::raw_buffer_handle buffer, cc::isize offset, cc::isize size);

    /// Requests a new staging window size in bytes (> 0), applied by the copy actor between windows: it
    /// drains every in-flight window, then rebuilds the staging buffer at `bytes * 3`. Thread-safe; the
    /// change is picked up before the next download is staged, so in-flight downloads are unaffected.
    void set_window_bytes(cc::isize bytes);

    /// Shuts the actor down (draining queued readbacks and waiting for the copy queue to idle), then unmaps
    /// and releases the staging buffer. Must run while the context's copy queue + completion fence and
    /// device are alive.
    void shutdown();

    // Set in initialize, then touched only by the copy actor (_staging/_mapped/_window_bytes are also
    // rebuilt by the actor when a set_window_bytes is applied) — the actor reads them lock-free.
    dx12_context& _ctx;
    ComPtr<ID3D12Resource> _staging;
    cc::byte* _mapped = nullptr;
    cc::isize _window_bytes = 0;
    ComPtr<ID3D12Fence> _window_fence; // per-window monotonic timeline: window reuse + one window's read done
    HANDLE _wait_event = nullptr;      // actor-thread wait on the window fence

    // A pending set_window_bytes request; the actor compares it to _window_bytes at the top of each
    // process cycle and rebuilds staging when they differ. Written by any thread, read by the actor.
    std::atomic<cc::isize> _desired_window_bytes = 0;

private:
    // Fringe: block the caller until a pending async upload to `src` (highest value on the context copy
    // fence) has completed, so the readback observes it. Warns; a documented v1 simplification.
    void wait_for_pending_async_upload(dx12_buffer const& src);

    // Reserved on the caller thread (fetch_add), handed out as dx12_download_fence_value; the actor's
    // windows signal the context completion fence up to the highest finished read value.
    std::atomic<cc::u64> _next_download_value = 0;

    cc::unique_ptr<cc::threaded_actor<dx12_async_download_job>> _actor;
};
} // namespace sg::backend::dx12
