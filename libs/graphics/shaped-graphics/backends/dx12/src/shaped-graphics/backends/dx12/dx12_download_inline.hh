#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

/// bytes_waiter for an inline download: ready once the download actor has copied the readback bytes into the destination.
/// wait() can only block after the recording list has been submitted — blocking earlier would deadlock the very thread that must submit.
/// A dropped recording list cancels the download: its future never becomes ready, and wait() reports failure.
class sg::backend::dx12::dx12_download_waiter final : public sg::bytes_waiter
{
public:
    /// Set true when the recording command list is submitted; gates wait().
    std::atomic_bool submitted = false;

    /// Marks the download cancelled: its list was dropped, so the copy will never run.
    /// wait() then fails instead of blocking forever.
    /// A cancelled download is never submitted, so this cannot race a thread blocked inside wait().
    void mark_cancelled() { _cancelled.store(true, std::memory_order_release); }

    [[nodiscard]] bool wait() override
    {
        if (_cancelled.load(std::memory_order_acquire))
            return false;
        if (!submitted.load(std::memory_order_acquire) && !is_ready())
            return false;
        _is_ready.wait(false, std::memory_order_acquire); // blocks until mark_ready() stores true
        return true;
    }

private:
    std::atomic_bool _cancelled = false;
};

/// One deferred readback copy, recorded token-less into a command list and enqueued on the download actor at submit.
/// The actor waits for `token` on the submission fence, then runs `deferred_cpu_copy` if `pin` is still alive.
/// A dropped future expires the pin and cancels the copy.
/// It then marks `waiter` ready and releases one count from `epoch_copies`, the per-epoch tally gating ring reclaim.
struct sg::backend::dx12::dx12_download_copy_job
{
    sg::submission_token token = sg::submission_token::not_submitted;
    cc::unique_function<void()> deferred_cpu_copy;
    std::weak_ptr<void const> pin;
    std::shared_ptr<dx12_download_waiter> waiter;

    /// The reserving epoch's outstanding-copy counter, held until this job is drained or its list is dropped.
    /// The epoch's ring span frees once the counter reaches zero.
    std::shared_ptr<std::atomic<isize>> epoch_copies;
};

/// Inline READBACK path: copies GPU buffer bytes back to the host through a persistently-mapped READBACK-heap ring on the direct queue.
/// The GPU copy is recorded inline, and a cc::threaded_actor then blocks on the submission fence and performs the CPU memcpy.
/// So a download completes without advancing the epoch.
/// Ring space is reclaimed at **epoch granularity**, never per submission: each epoch carries an outstanding-copy counter, and its whole span frees when that counter hits zero.
/// Why that coarsening is load-bearing under concurrent recording: libs/graphics/shaped-graphics/docs/concepts/download.inline.md.
class sg::backend::dx12::dx12_download_inline_system
{
public:
    explicit dx12_download_inline_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates + persistently maps the READBACK ring buffer (`capacity` > 0), creates the actor's wait event, and starts the download actor.
    /// Called once during context bring-up.
    /// Returns a dx12 error when a resource could not be created.
    [[nodiscard]] cc::result<cc::unit> initialize(isize capacity);

    /// Records a readback of [offset, offset+size) from `src` and returns the pending future.
    /// Appends the deferred copy token-less to `cmd`; the context stamps and enqueues it at submit.
    /// A zero-size read returns an already-ready, empty future.
    [[nodiscard]] sg::bytes_future download_buffer(dx12_command_list& cmd, dx12_buffer const& src, isize offset, isize size);

    /// Records a readback of one texture region (per `fp`) from `src` and returns the pending future.
    /// A region larger than the free ring, or one straddling the seam, splits into several copies.
    /// Each chunk's deferred CPU copy un-pads its rows into the future's tightly-packed host buffer.
    /// The caller emits the copy_src layout barrier first.
    [[nodiscard]] sg::bytes_future download_texture(dx12_command_list& cmd,
                                                    ID3D12Resource* src,
                                                    dx12_texture_footprint const& fp);

    /// Stamps `jobs` with `token`, marks their waiters submitted, and enqueues them on the actor in order.
    /// Called from submit while the submission order is held.
    void enqueue_submitted(sg::submission_token token, cc::vector<dx12_download_copy_job>& jobs);

    /// Cancels a dropped (never-submitted) list's downloads: marks each future cancelled and releases its epoch-copy count.
    /// The epoch can then still reclaim once its submitted downloads drain.
    /// The reserved bytes are not freed here — they belong to the open epoch's span and are reclaimed with it.
    void discard_unsubmitted(cc::vector<dx12_download_copy_job>& jobs);

    /// Closes epoch `closed`: snapshots the ring cursor as its boundary and captures its outstanding-copy counter.
    /// The span then frees once the actor has drained those copies.
    /// Called at advance.
    void on_epoch_advance(sg::epoch closed);

    /// Records a pending ring capacity (> 0), applied at the next epoch boundary (apply_pending_budget).
    void set_budget(isize capacity);

    /// Applies a pending set_budget at an epoch boundary, and is a no-op when nothing is pending.
    /// Drains every in-flight epoch, then waits for the actor to finish every outstanding readback copy — each reads the *old* ring.
    /// Only then does it drop and rebuild the ring at the new capacity.
    /// Called from advance_epoch.
    void apply_pending_budget();

    /// Blocks the actor until `token` has completed on the submission fence.
    void wait_for_submission(sg::submission_token token);

    /// Releases one outstanding copy from `epoch_copies` and reclaims any now-fully-drained epochs.
    /// Called by the actor after each copy.
    void on_copy_done(std::shared_ptr<std::atomic<isize>> const& epoch_copies);

    /// Shuts the actor down (draining pending copies), then unmaps + releases the ring buffer.
    void shutdown();

    /// Runs one cycle of the copy actor on the calling thread; true if there may be more work.
    /// A no-op returning false wherever the actor has its own thread — see sg::context::pump.
    bool pump_unthreaded() { return _actor != nullptr && _actor->process_messages_if_unthreaded(); }

    // --- test-only escape hatches --------------------------------------------------------------------
    // Backend tests peel the abstraction to assert ring-cursor behavior, e.g. seam-splitting.
    // Not part of the production surface — see libs/graphics/shaped-graphics/docs/testing.md.

    /// A snapshot of the ring's logical cursors and physical capacity.
    struct debug_cursor_snapshot
    {
        u64 next_pos = 0;
        u64 freed_pos = 0;
        isize capacity = 0;
    };
    [[nodiscard]] debug_cursor_snapshot debug_cursor();

    /// Repositions the logical cursor at `pos`, so the next reserve starts at physical `pos % capacity`.
    /// Sets next_pos == freed_pos == pos and clears checkpoints, so the ring reads as empty at that seam-relative position.
    /// Call only on an already-drained ring, with no outstanding copies.
    void debug_set_cursor(u64 pos);

private:
    /// A one-shot span reservation (see reserve_span): the start cursor of `total` contiguous logical bytes, which may wrap the seam.
    /// Carries the open epoch's copy counter its chunks are accounted against.
    struct span_reservation
    {
        u64 start = 0;
        std::shared_ptr<std::atomic<isize>> epoch_copies;
    };

    /// Reserves `total` contiguous logical bytes in one shot and returns its start cursor plus the open epoch's counter.
    /// The span may wrap the physical seam; the caller walks it, handing a resumable readback to-seam windows (offset `cursor % capacity`, size to the seam).
    /// Does not itself count a copy — call account_pending_copy per window that yields a pushed copy job.
    /// A self-aligning texture readback can hit a seam tail that makes no progress, and that must not be counted.
    /// `total` must fit the capacity.
    /// Blocks on the reclaim watermark while space is held by earlier, still-in-flight epochs.
    span_reservation reserve_span(isize total);

    /// Counts one copy against the open epoch's tally (`epoch_copies`) and the global drain gate.
    /// Call exactly once per pushed dx12_download_copy_job; on_copy_done / discard_unsubmitted release it.
    void account_pending_copy(std::shared_ptr<std::atomic<isize>> const& epoch_copies);

    /// Blocks the calling thread until the actor has drained every outstanding readback copy.
    /// That is, until every accounted copy has been matched by an on_copy_done or a discard.
    /// Used by apply_pending_budget before it frees the ring the actor's copies read from.
    void wait_until_idle();

    /// A closed epoch's ring boundary plus its outstanding-copy counter; the span [.., end_pos) frees once `outstanding` reaches zero.
    struct epoch_checkpoint
    {
        sg::epoch epoch_id = sg::epoch::invalid;
        u64 end_pos = 0;
        std::shared_ptr<std::atomic<isize>> outstanding;
    };

    struct ring_state
    {
        u64 next_pos = 0;                                         // logical bump cursor over the u64 space
        std::shared_ptr<std::atomic<isize>> current_epoch_copies; // counter for the open epoch
        cc::vector<epoch_checkpoint> checkpoints;                 // FIFO, oldest epoch at the front
        isize pending_capacity = 0;                               // a set_budget awaiting the next boundary (0 = none)

        /// Advances `sys`'s free watermark over every leading checkpoint whose copies have all drained, then wakes waiting reservers.
        /// Only reachable while the `_ring` lock is held, since it takes a ring_state&.
        /// `sys` supplies the atomic watermark that lives outside the lock.
        void reclaim(dx12_download_inline_system& sys);
    };

    dx12_context& _ctx;

    ComPtr<ID3D12Resource> _buffer;
    byte* _mapped = nullptr;
    isize _capacity = 0;
    HANDLE _wait_event = nullptr;

    std::atomic<u64> _freed_pos = 0; // reclaim watermark; advanced by reclaim, waited on by reserve

    // Total readback copies reserved but not yet drained, across all epochs.
    // Bumped by account_pending_copy, dropped in on_copy_done / discard.
    // A resize waits on it reaching zero to know the actor no longer reads the old ring.
    // A single global counter is what makes wait_until_idle race-free; polling per-epoch state would race the actor.
    std::atomic<isize> _outstanding = 0;

    cc::mutex<ring_state> _ring;

    cc::unique_ptr<cc::threaded_actor<dx12_download_copy_job>> _actor;
};
