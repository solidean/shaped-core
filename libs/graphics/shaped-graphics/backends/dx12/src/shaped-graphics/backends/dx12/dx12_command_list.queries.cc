// dx12_command_list: GPU-query recording (cmd.query). Timestamps lease query heaps from the context's
// dx12_query_system, and finalize_queries_before_close resolves + reads them back at submit. See
// libs/graphics/shaped-graphics/docs/concepts/queries.md.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

#include <memory>

namespace sg::backend::dx12
{
bool dx12_command_list::query_timestamps_supported() const
{
    return _ctx._query_system.supports_timestamps();
}

sg::gpu_timestamp dx12_command_list::query_record_gpu_timestamp()
{
    if (!_ctx._query_system.supports_timestamps())
        return {}; // invalid timestamp — caller reads is_valid()/is_supported()

    // Need a fresh lease if none is active or the active one is full.
    bool const need_fresh = _active_timestamp_lease < 0
                         || _leased_query_heaps[_active_timestamp_lease]->next_slot
                                >= _leased_query_heaps[_active_timestamp_lease]->slot_count;
    if (need_fresh)
    {
        _active_timestamp_lease = int(_leased_query_heaps.size());
        _leased_query_heaps.push_back(_ctx._query_system.acquire_heap(dx12_query_heap_type::timestamp));
    }

    auto& lease = *_leased_query_heaps[_active_timestamp_lease];
    int const slot = lease.next_slot++;

    // A timestamp is a point-in-time value: recorded via EndQuery alone (no BeginQuery).
    _list->EndQuery(lease.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot));

    // The handle aliases the heap's shared future (filled at submit) and indexes its own slot within it.
    return sg::gpu_timestamp(std::shared_ptr<sg::data_future<cc::u64> const>(lease.shared_future), cc::isize(slot),
                             _ctx._query_system.timestamp_tick_to_seconds());
}

void dx12_command_list::finalize_queries_before_close()
{
    // Heaps are leased on demand, so every leased heap holds at least one recorded query — empty == no work.
    if (_leased_query_heaps.empty())
        return;

    // 1. Total slots across all leased heaps → resolve-buffer size (one u64 per slot).
    cc::isize total_slots = 0;
    for (auto const& lease : _leased_query_heaps)
        total_slots += lease->next_slot;
    CC_ASSERT(total_slots > 0, "leased query heaps with zero recorded queries");
    cc::isize const total_bytes = total_slots * cc::isize(sizeof(cc::u64));

    // 2. Transient buffer to receive the resolved ticks: a ResolveQueryData target (copy_dst) that the
    //    inline readback then reads (copy_src). Recycled once this epoch retires — which cannot happen
    //    before the resolve + readback copies of this epoch complete on the direct queue.
    auto const resolve_raw
        = _ctx.transient.create_raw_buffer(total_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto const resolve = std::dynamic_pointer_cast<dx12_buffer const>(resolve_raw);
    CC_ASSERT(resolve != nullptr, "transient resolve buffer is not a dx12 buffer");

    // 3. Declare the whole buffer COPY_DEST and flush, then resolve every leased heap into its slice.
    track_buffer_access(resolve, sg::pipeline_stage_flags::copy, sg::access_flags::copy_write);
    flush_barriers();
    cc::isize offset_bytes = 0;
    for (auto const& lease : _leased_query_heaps)
    {
        CC_ASSERT(lease->next_slot > 0, "leased query heap should have at least one recorded query");
        _list->ResolveQueryData(lease->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, UINT(lease->next_slot),
                                resolve->_resource.Get(), UINT64(offset_bytes));
        offset_bytes += cc::isize(lease->next_slot) * cc::isize(sizeof(cc::u64));
    }

    // 4. Barrier COPY_DEST→COPY_SOURCE, then start one inline readback per heap into its slice and assign
    //    the heap's shared future in place so handed-out handles see it.
    track_buffer_access(resolve, sg::pipeline_stage_flags::copy, sg::access_flags::copy_read);
    flush_barriers();
    offset_bytes = 0;
    for (auto& lease : _leased_query_heaps)
    {
        cc::isize const count = lease->next_slot;
        cc::isize const size_bytes = count * cc::isize(sizeof(cc::u64));
        auto bytes = _ctx._download_inline.download_buffer(*this, *resolve, offset_bytes, size_bytes);
        *lease->shared_future = sg::data_future<cc::u64>(cc::move(bytes));
        offset_bytes += size_bytes;
    }

    // 5. Return every lease to the pool and reset the active-lease index.
    for (auto& lease : _leased_query_heaps)
        _ctx._query_system.release_heap(cc::move(lease));
    _leased_query_heaps.clear();
    _active_timestamp_lease = -1;
}
} // namespace sg::backend::dx12
