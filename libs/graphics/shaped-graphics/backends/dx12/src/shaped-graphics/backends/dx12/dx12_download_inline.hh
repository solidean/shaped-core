#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>
#include <memory>

namespace sg::backend::dx12
{
/// bytes_waiter for an inline download: ready once the download actor has copied the readback bytes
/// into the destination. wait() can only block after the recording list has been submitted (otherwise
/// the actor has no work queued and blocking would deadlock the submitting thread).
class dx12_download_waiter final : public sg::bytes_waiter
{
public:
    /// Set true when the recording command list is submitted; gates wait().
    std::atomic_bool submitted = false;

    [[nodiscard]] bool wait() override
    {
        if (!submitted.load(std::memory_order_acquire) && !is_ready())
            return false;
        _is_ready.wait(false, std::memory_order_acquire); // blocks until mark_ready() stores true
        return true;
    }
};

/// One deferred readback copy, enqueued on the download actor at submit time and processed in submit
/// order. The actor waits for `token` on the submission fence, runs `deferred_cpu_copy` if `pin` is
/// still alive (dropping the future cancels the copy), marks `waiter` ready, and frees the ring space
/// up to `end_pos`.
struct dx12_download_copy_job
{
    sg::submission_token token = sg::submission_token::not_submitted;
    cc::unique_function<void()> deferred_cpu_copy;
    std::weak_ptr<void> pin;
    std::shared_ptr<dx12_download_waiter> waiter;
    cc::u64 end_pos = 0;
};

/// Inline READBACK path: copies GPU buffer bytes back to the host through a persistently-mapped
/// READBACK-heap ring buffer on the direct queue. The GPU copy is recorded inline; a cc::threaded_actor
/// then blocks on the submission fence and performs the CPU memcpy, so a download completes without
/// advancing the epoch. Ring space frees once the actor finishes each copy.
class dx12_download_inline_system
{
public:
    explicit dx12_download_inline_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Takes ownership of the persistently-mapped ring buffer + wait event and starts the actor.
    void initialize(ComPtr<ID3D12Resource> buffer, cc::byte* mapped, cc::isize capacity, HANDLE wait_event);

    /// Records a readback of [offset, offset+size) from `src` and returns the pending future. Appends
    /// the deferred copy (token-less) to `cmd`; the context stamps + enqueues it at submit. A zero-size
    /// read returns an already-ready, empty future.
    [[nodiscard]] sg::bytes_future download_buffer(dx12_command_list& cmd,
                                                   dx12_buffer const& src,
                                                   cc::isize offset,
                                                   cc::isize size);

    /// Stamps `jobs` with `token`, marks their waiters submitted, and enqueues them on the actor in
    /// order. Called from submit while the submission order is held.
    void enqueue_submitted(sg::submission_token token, cc::vector<dx12_download_copy_job>& jobs);

    /// Reclaims ring space reserved by a dropped (never-submitted) list's downloads. Their futures
    /// never become ready.
    void discard_unsubmitted(cc::vector<dx12_download_copy_job>& jobs);

    /// Blocks the actor until `token` has completed on the submission fence.
    void wait_for_submission(sg::submission_token token);

    /// Frees ring space up to `end_pos` (called by the actor after a copy). Monotonic.
    void advance_free_watermark(cc::u64 end_pos);

    /// Shuts the actor down (draining pending copies), then unmaps + releases the ring buffer.
    void shutdown();

private:
    /// Reserves a contiguous, non-wrapping window; returns {physical offset, logical end}. Blocks on
    /// the actor's free watermark when space is held by earlier, still-in-flight downloads.
    struct reservation
    {
        cc::isize offset = 0;
        cc::u64 end_pos = 0;
    };
    reservation reserve(cc::isize size);

    dx12_context& _ctx;

    ComPtr<ID3D12Resource> _buffer;
    cc::byte* _mapped = nullptr;
    cc::isize _capacity = 0;
    HANDLE _wait_event = nullptr;

    std::atomic<cc::u64> _freed_pos = 0; // advanced by the actor; waited on by reserve

    struct ring_state
    {
        cc::u64 next_pos = 0;
    };
    cc::mutex<ring_state> _ring;

    cc::unique_ptr<cc::threaded_actor<dx12_download_copy_job>> _actor;
};
} // namespace sg::backend::dx12
