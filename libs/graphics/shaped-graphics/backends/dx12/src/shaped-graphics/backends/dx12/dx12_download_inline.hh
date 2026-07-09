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
#include <memory>

namespace sg::backend::dx12
{
/// bytes_waiter for an inline download: ready once the download actor has copied the readback bytes
/// into the destination. wait() can only block after the recording list has been submitted (otherwise
/// the actor has no work queued and blocking would deadlock the submitting thread). A dropped recording
/// list cancels the download — its future never becomes ready and wait() reports failure.
class dx12_download_waiter final : public sg::bytes_waiter
{
public:
    /// Set true when the recording command list is submitted; gates wait().
    std::atomic_bool submitted = false;

    /// Marks the download cancelled: its list was dropped, so the copy will never run. wait() then
    /// fails instead of blocking forever. A cancelled download is never submitted, so this cannot race
    /// a thread blocked inside wait() (that only blocks once submitted is true).
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

/// One deferred readback copy. Recorded (token-less) into a command list, then enqueued on the
/// download actor at submit. The actor waits for `token` on the submission fence, runs
/// `deferred_cpu_copy` if `pin` is still alive (dropping the future cancels the copy), marks `waiter`
/// ready, and releases one count from `epoch_copies` — the per-epoch tally that gates ring reclaim.
struct dx12_download_copy_job
{
    sg::submission_token token = sg::submission_token::not_submitted;
    cc::unique_function<void()> deferred_cpu_copy;
    std::weak_ptr<void const> pin;
    std::shared_ptr<dx12_download_waiter> waiter;

    /// The reserving epoch's outstanding-copy counter. Held until this job is drained (or its list is
    /// dropped); the epoch's ring span frees once the counter reaches zero.
    std::shared_ptr<std::atomic<cc::isize>> epoch_copies;
};

/// Inline READBACK path: copies GPU buffer bytes back to the host through a persistently-mapped
/// READBACK-heap ring buffer on the direct queue. The GPU copy is recorded inline; a cc::threaded_actor
/// then blocks on the submission fence and performs the CPU memcpy, so a download completes without
/// advancing the epoch.
///
/// Ring space is reclaimed at **epoch granularity**. Reservation is per-download (concurrent, in
/// allocation order), and the actor drains the CPU copies in submission order — but reclaiming a
/// window is only safe once *every* download of the epoch that reserved it has drained. Multiple lists
/// record concurrently, so submission order need not match allocation order; freeing per submission
/// could release a window an unsubmitted list still holds. Each epoch therefore carries an
/// outstanding-copy counter; its whole span frees when the counter hits zero. See
/// libs/graphics/shaped-graphics/docs/concepts/download.inline.md.
class dx12_download_inline_system
{
public:
    explicit dx12_download_inline_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates + persistently maps the READBACK ring buffer (capacity bytes, > 0), creates the actor's
    /// wait event, and starts the download actor. Called once during context bring-up. Returns a dx12
    /// error if a resource could not be created.
    [[nodiscard]] cc::result<cc::unit> initialize(cc::isize capacity);

    /// Records a readback of [offset, offset+size) from `src` and returns the pending future. Appends
    /// the deferred copy (token-less) to `cmd`; the context stamps + enqueues it at submit. A zero-size
    /// read returns an already-ready, empty future.
    [[nodiscard]] sg::bytes_future download_buffer(dx12_command_list& cmd,
                                                   dx12_buffer const& src,
                                                   cc::isize offset,
                                                   cc::isize size);

    /// Records a readback of one texture region (per `fp`) from `src`, packing it into 512-aligned readback
    /// windows row/slice-wise (a region larger than the free ring, or one straddling the seam, splits into
    /// several copies), and returns the pending future. Each chunk's deferred CPU copy un-pads its rows into
    /// the future's tightly-packed host buffer. The caller emits the copy_src layout barrier first.
    [[nodiscard]] sg::bytes_future download_texture(dx12_command_list& cmd,
                                                    ID3D12Resource* src,
                                                    dx12_texture_footprint const& fp);

    /// Stamps `jobs` with `token`, marks their waiters submitted, and enqueues them on the actor in
    /// order. Called from submit while the submission order is held.
    void enqueue_submitted(sg::submission_token token, cc::vector<dx12_download_copy_job>& jobs);

    /// Cancels a dropped (never-submitted) list's downloads: marks each future cancelled and releases
    /// its epoch-copy count so the epoch can still reclaim once its submitted downloads drain. The
    /// reserved bytes are not freed here — they belong to the open epoch's span, reclaimed with it.
    void discard_unsubmitted(cc::vector<dx12_download_copy_job>& jobs);

    /// Closes epoch `closed`: snapshots the ring cursor as its boundary and captures its outstanding-
    /// copy counter, so the span frees once the actor drains those copies. Called at advance.
    void on_epoch_advance(sg::epoch closed);

    /// Records a pending ring capacity (> 0), applied at the next epoch boundary (apply_pending_budget).
    void set_budget(cc::isize capacity);

    /// Applies a pending set_budget at an epoch boundary: drains every in-flight epoch, then waits for the
    /// download actor to finish every outstanding readback copy (each reads the *old* ring) before dropping
    /// and rebuilding it at the new capacity. No-op if nothing is pending. Called from advance_epoch.
    void apply_pending_budget();

    /// Blocks the actor until `token` has completed on the submission fence.
    void wait_for_submission(sg::submission_token token);

    /// Releases one outstanding copy from `epoch_copies` and reclaims any now-fully-drained epochs.
    /// Called by the actor after each copy.
    void on_copy_done(std::shared_ptr<std::atomic<cc::isize>> const& epoch_copies);

    /// Shuts the actor down (draining pending copies), then unmaps + releases the ring buffer.
    void shutdown();

    // --- test-only escape hatches --------------------------------------------------------------------
    // Backend tests peel the abstraction to assert ring-cursor behavior (e.g. seam-splitting). See
    // libs/graphics/shaped-graphics/docs/testing.md. Not part of the production surface.

    /// A snapshot of the ring's logical cursors and physical capacity.
    struct debug_cursor_snapshot
    {
        cc::u64 next_pos = 0;
        cc::u64 freed_pos = 0;
        cc::isize capacity = 0;
    };
    [[nodiscard]] debug_cursor_snapshot debug_cursor();

    /// Repositions the logical cursor at `pos` (so the next reserve starts at physical `pos % capacity`)
    /// on an already-drained ring: sets next_pos == freed_pos == pos and clears checkpoints, so the ring
    /// reads as empty at that seam-relative position. Call only after a full drain (no outstanding copies).
    void debug_set_cursor(cc::u64 pos);

private:
    /// A contiguous, non-wrapping ring slice: physical `offset`, the `granted` bytes there (capped at
    /// the seam, so a wrapping request is handed back in pieces the caller loops over), and a handle to
    /// the open epoch's copy counter (not yet incremented — see account_pending_copy). Blocks on the
    /// reclaim watermark when space is held by earlier, still-in-flight epochs.
    struct reservation
    {
        cc::isize offset = 0;
        cc::isize granted = 0;
        std::shared_ptr<std::atomic<cc::isize>> epoch_copies;
    };

    /// A one-shot span reservation (see reserve_span): the start cursor of `total` contiguous logical bytes
    /// (may wrap the seam) plus the open epoch's copy counter its chunks are accounted against.
    struct span_reservation
    {
        cc::u64 start = 0;
        std::shared_ptr<std::atomic<cc::isize>> epoch_copies;
    };

    /// Reserves up to `size` bytes at the current cursor, never crossing the physical seam (the result
    /// may be smaller than `size`). Does not itself count a copy — call account_pending_copy once the
    /// reservation actually yields a pushed copy job (a self-aligning texture readback may reserve a seam
    /// tail that makes no progress, which must not be counted).
    reservation reserve(cc::isize size);

    /// Reserves `total` contiguous logical bytes in one shot (the span may wrap the physical seam) and
    /// returns its start cursor + the open epoch's counter; the caller walks it, handing a resumable readback
    /// to-seam windows. Used by the texture path, which needs one window big enough to self-align + make
    /// progress. `total` must fit the capacity. Blocks like reserve().
    span_reservation reserve_span(cc::isize total);

    /// Counts one copy against the open epoch's tally (`epoch_copies`) and the global drain gate. Call
    /// exactly once per pushed dx12_download_copy_job; on_copy_done / discard_unsubmitted release it.
    void account_pending_copy(std::shared_ptr<std::atomic<cc::isize>> const& epoch_copies);

    /// Blocks the calling thread until the actor has drained every outstanding readback copy (i.e. every
    /// reserve has been matched by an on_copy_done or a discard). Used by apply_pending_budget before it
    /// frees the ring the actor's copies read from.
    void wait_until_idle();

    /// A closed epoch's ring boundary + its outstanding-copy counter; its span [.., end_pos) frees
    /// once `outstanding` reaches zero.
    struct epoch_checkpoint
    {
        sg::epoch epoch_id = sg::epoch::invalid;
        cc::u64 end_pos = 0;
        std::shared_ptr<std::atomic<cc::isize>> outstanding;
    };

    struct ring_state
    {
        cc::u64 next_pos = 0;                                         // logical bump cursor over the u64 space
        std::shared_ptr<std::atomic<cc::isize>> current_epoch_copies; // counter for the open epoch
        cc::vector<epoch_checkpoint> checkpoints;                     // FIFO, oldest epoch at the front
        cc::isize pending_capacity = 0; // a set_budget awaiting the next boundary (0 = none)

        /// Advances `sys`'s free watermark over every leading checkpoint whose copies have all drained,
        /// then wakes waiting reservers. Only reachable while the `_ring` lock is held (it takes a
        /// ring_state&); `sys` supplies the atomic watermark that lives outside the lock.
        void reclaim(dx12_download_inline_system& sys);
    };

    dx12_context& _ctx;

    ComPtr<ID3D12Resource> _buffer;
    cc::byte* _mapped = nullptr;
    cc::isize _capacity = 0;
    HANDLE _wait_event = nullptr;

    std::atomic<cc::u64> _freed_pos = 0; // reclaim watermark; advanced by reclaim, waited on by reserve

    // Total readback copies reserved but not yet drained (across all epochs). Bumped in reserve, dropped
    // in on_copy_done / discard; a resize waits on it reaching zero to know the actor no longer reads the
    // old ring. A single global counter makes wait_until_idle race-free (unlike polling per-epoch state).
    std::atomic<cc::isize> _outstanding = 0;

    cc::mutex<ring_state> _ring;

    cc::unique_ptr<cc::threaded_actor<dx12_download_copy_job>> _actor;
};
} // namespace sg::backend::dx12
