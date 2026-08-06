#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// Inline UPLOAD path: stages CPU→GPU writes through a persistently-mapped UPLOAD-heap ring on the direct queue.
/// The copy is recorded immediately, so the destination is usable by later commands in the same list.
/// An epoch's ring space is reclaimed once that epoch's GPU work retires.
/// See libs/graphics/shaped-graphics/docs/concepts/upload.inline.md.
class dx12_upload_inline_system
{
public:
    explicit dx12_upload_inline_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Creates + persistently maps the UPLOAD ring buffer; `capacity` must be > 0.
    /// Called once during context bring-up.
    /// Returns a dx12 error when the resource or the mapping could not be created.
    [[nodiscard]] cc::result<cc::unit> initialize(isize capacity);

    /// Stages `data` into `dst` at `dst_offset`, recording the copy into `cmd`.
    /// Synchronous: the source bytes are consumed before returning.
    /// Empty `data` is a no-op.
    void upload_buffer(dx12_command_list& cmd, dx12_buffer const& dst, cc::span<byte const> data, isize dst_offset);

    /// Stages one texture region's tightly-packed `data` into `dst` per `fp`, recording CopyTextureRegion(s) into `cmd`.
    /// A region larger than the free ring space, or one straddling the seam, splits into several copies.
    /// Unsupported: a single padded row wider than the whole ring.
    /// The caller emits the copy_dst layout barrier first — it is the one holding the sg texture.
    /// Synchronous: `data` is consumed before returning.
    void upload_texture(dx12_command_list& cmd,
                        ID3D12Resource* dst,
                        dx12_texture_footprint const& fp,
                        cc::span<byte const> data);

    /// Snapshots the ring cursor as the end-of-epoch boundary for `closed` (called at advance).
    void on_epoch_advance(sg::epoch closed);

    /// Advances the free watermark past every epoch <= `completed` (called at retire).
    void on_epochs_completed(sg::epoch completed);

    /// Records a pending ring capacity (> 0), applied at the next epoch boundary (apply_pending_budget).
    void set_budget(isize capacity);

    /// Applies a pending set_budget at an epoch boundary, and is a no-op when nothing is pending.
    /// Drains every in-flight epoch so no GPU work still reads the ring, then reallocates it at the new capacity.
    /// Called from advance_epoch once the new epoch is open.
    void apply_pending_budget();

    /// Unmaps + releases the ring buffer.
    void shutdown();

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
    /// Call only on an already-drained ring.
    void debug_set_cursor(u64 pos);

private:
    /// Reserves `total` contiguous logical bytes in one shot and returns its start cursor.
    /// The span may wrap the physical seam; the caller walks it, handing a resumable job to-seam windows (offset `cursor % capacity`, size to the seam).
    /// `total` must fit the capacity.
    /// Blocks, retiring in-flight epochs, while the space is still held by earlier ones.
    u64 reserve_span(isize total);

    dx12_context& _ctx;

    ComPtr<ID3D12Resource> _buffer;
    byte* _mapped = nullptr;
    isize _capacity = 0;

    /// A logical end-cursor snapshot for a closed epoch; its space frees once the epoch retires.
    struct epoch_checkpoint
    {
        sg::epoch epoch_id = sg::epoch::invalid;
        u64 end_pos = 0;
    };

    struct ring_state
    {
        u64 next_pos = 0;                         // logical bump cursor over the u64 space
        u64 freed_pos = 0;                        // everything logically below this is reclaimable
        cc::vector<epoch_checkpoint> checkpoints; // FIFO, oldest epoch at the front
        isize pending_capacity = 0;               // a set_budget awaiting the next epoch boundary (0 = none)
    };
    cc::mutex<ring_state> _ring;
};
} // namespace sg::backend::dx12
