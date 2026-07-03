#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// Inline UPLOAD path: stages latency-critical CPU→GPU buffer writes through a persistently-mapped
/// UPLOAD-heap ring buffer on the direct queue. An upload memcpys into the ring and records a
/// CopyBufferRegion immediately, so the destination is usable by later commands in the same list.
/// Ring space for an epoch's uploads is reclaimed once that epoch's GPU work retires.
class dx12_upload_inline_system
{
public:
    explicit dx12_upload_inline_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Takes ownership of the persistently-mapped ring buffer. Called once during context bring-up.
    void initialize(ComPtr<ID3D12Resource> buffer, cc::byte* mapped, cc::isize capacity);

    /// Stages `data` into `dst` at `dst_offset`, recording the copy into `cmd`. Synchronous: the
    /// source bytes are consumed before returning. Empty `data` is a no-op.
    void upload_buffer(dx12_command_list& cmd, dx12_buffer const& dst, cc::span<cc::byte const> data, cc::isize dst_offset);

    /// Snapshots the ring cursor as the end-of-epoch boundary for `closed` (called at advance).
    void on_epoch_advance(sg::epoch closed);

    /// Advances the free watermark past every epoch <= `completed` (called at retire).
    void on_epochs_completed(sg::epoch completed);

    /// Unmaps + releases the ring buffer.
    void shutdown();

private:
    /// Reserves a contiguous, non-wrapping window of `size` bytes and returns its physical offset into
    /// the mapped buffer. Blocks (retiring in-flight epochs) when the space is still held by earlier
    /// epochs. Asserts if a single upload exceeds the ring capacity.
    cc::isize reserve(cc::isize size);

    dx12_context& _ctx;

    ComPtr<ID3D12Resource> _buffer;
    cc::byte* _mapped = nullptr;
    cc::isize _capacity = 0;

    /// A logical end-cursor snapshot for a closed epoch; its space frees once the epoch retires.
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
