#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// Transient BUFFER pool: a linear ring over one DEFAULT memory_heap. Each transient buffer is placed
/// into the heap at a bump offset; the window an epoch consumes is reclaimed once that epoch retires —
/// memory reuse is safe because the epoch fence has passed by then. This mirrors the inline
/// upload/download rings (logical u64 cursor, per-epoch checkpoints, non-wrapping windows), but hands
/// out heap offsets for placed resources instead of mapped bytes.
///
/// Requests larger than the whole heap fall back to a committed (dedicated) resource. Every allocation
/// carries lifetime_scope::transient.
class dx12_transient_buffer_allocator
{
public:
    explicit dx12_transient_buffer_allocator(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates the DEFAULT heap the ring bumps over. `capacity` (> 0) is rounded up to the placement
    /// alignment. Called once during context bring-up.
    [[nodiscard]] cc::result<cc::unit> initialize(cc::isize capacity);

    /// Allocates a transient buffer from the current epoch's ring window (committed fallback when the
    /// request exceeds the heap). Size must be >= 0 (0 is a valid empty buffer, no placement).
    [[nodiscard]] cc::result<dx12_buffer_handle> create_buffer(cc::isize size_in_bytes, sg::buffer_usage usage);

    /// Snapshots the ring cursor as the end-of-epoch boundary for `closed` (called at advance).
    void on_epoch_advance(sg::epoch closed);

    /// Advances the free watermark past every epoch <= `completed` (called at retire).
    void on_epochs_completed(sg::epoch completed);

    /// Releases the heap.
    void shutdown();

private:
    /// Reserves a contiguous, non-wrapping window of `size` bytes and returns its heap offset. Blocks
    /// (retiring in-flight epochs) when the space is still held by earlier epochs. `size` must be a
    /// multiple of the placement alignment and <= capacity.
    cc::isize reserve(cc::isize size);

    dx12_context& _ctx;
    sg::memory_heap_handle _heap;
    cc::isize _capacity = 0; // heap size in bytes; a multiple of the placement alignment

    /// A logical end-cursor snapshot for a closed epoch; its window frees once the epoch retires.
    struct epoch_checkpoint
    {
        sg::epoch epoch_id = sg::epoch::invalid;
        cc::u64 end_pos = 0;
    };

    struct ring_state
    {
        cc::u64 next_pos = 0;                     // logical bump cursor over the u64 space
        cc::u64 freed_pos = 0;                    // everything logically below this is reclaimable
        cc::vector<epoch_checkpoint> checkpoints; // FIFO, oldest epoch at the front
    };
    cc::mutex<ring_state> _ring;
};
} // namespace sg::backend::dx12
