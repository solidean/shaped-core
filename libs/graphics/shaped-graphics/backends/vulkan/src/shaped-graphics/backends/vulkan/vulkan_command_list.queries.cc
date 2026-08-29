// vulkan_command_list: GPU-query recording (cmd.query).
// Timestamps lease query pools from the context's vulkan_query_system, and finalize_queries_before_close resolves +
// reads them back at submit.
// See libs/graphics/shaped-graphics/docs/concepts/queries.md.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg::backend::vulkan
{
bool vulkan_command_list::query_timestamps_supported() const
{
    return _ctx._query_system.supports_timestamps();
}

sg::gpu_timestamp vulkan_command_list::query_record_gpu_timestamp()
{
    if (!_ctx._query_system.supports_timestamps())
        return {}; // invalid timestamp — the caller reads is_valid() / is_supported()

    bool const need_fresh = _active_timestamp_lease < 0
                         || _leased_query_pools[_active_timestamp_lease]->next_slot
                                >= _leased_query_pools[_active_timestamp_lease]->slot_count;
    if (need_fresh)
    {
        _active_timestamp_lease = int(_leased_query_pools.size());
        _leased_query_pools.push_back(_ctx._query_system.acquire_pool());
    }

    auto& lease = *_leased_query_pools[_active_timestamp_lease];
    int const slot = lease.next_slot++;

    // BOTTOM_OF_PIPE: the timestamp is written once everything recorded before it has completed, which is what makes
    // the difference between two of them the duration of the work between.
    vkCmdWriteTimestamp2(_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, lease.pool, u32(slot));

    return sg::gpu_timestamp(std::shared_ptr<sg::data_future<u64> const>(lease.shared_future), isize(slot),
                             _ctx._query_system.timestamp_tick_to_seconds());
}

void vulkan_command_list::finalize_queries_before_close()
{
    // Pools are leased on demand, so every leased pool holds at least one recorded query — empty means no work.
    if (_leased_query_pools.empty())
        return;

    isize total_slots = 0;
    for (auto const& lease : _leased_query_pools)
        total_slots += lease->next_slot;
    CC_ASSERT(total_slots > 0, "leased query pools with zero recorded queries");
    isize const total_bytes = total_slots * isize(sizeof(u64));

    // A transient buffer to receive the resolved ticks: written by the copy below, then read by the inline readback.
    // Recycled once this epoch retires, which cannot happen before both of those complete.
    auto const resolve_raw
        = _ctx.transient.create_raw_buffer(total_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto const resolve = std::dynamic_pointer_cast<vulkan_buffer const>(resolve_raw);
    CC_ASSERT(resolve != nullptr, "transient resolve buffer is not a vulkan buffer");

    track_buffer_access(*resolve, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    flush_barriers();

    isize offset_bytes = 0;
    for (auto const& lease : _leased_query_pools)
    {
        CC_ASSERT(lease->next_slot > 0, "leased query pool should have at least one recorded query");
        // WAIT makes the copy itself block until every query in the range is available, which is what lets the
        // readback below be an ordinary buffer copy with no second synchronization step.
        vkCmdCopyQueryPoolResults(_buffer, lease->pool, 0, u32(lease->next_slot), resolve->_buffer,
                                  VkDeviceSize(offset_bytes), VkDeviceSize(sizeof(u64)),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        offset_bytes += isize(lease->next_slot) * isize(sizeof(u64));
    }

    track_buffer_access(*resolve, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
    flush_barriers();

    // One inline readback per pool into its slice, with the pool's shared future assigned in place so the handles
    // already handed out see it.
    offset_bytes = 0;
    for (auto& lease : _leased_query_pools)
    {
        isize const size_bytes = isize(lease->next_slot) * isize(sizeof(u64));
        auto bytes = download_bytes_from_buffer(resolve_raw, offset_bytes, size_bytes);
        *lease->shared_future = sg::data_future<u64>(cc::move(bytes));
        offset_bytes += size_bytes;
    }

    for (auto& lease : _leased_query_pools)
        _ctx._query_system.release_pool(cc::move(lease));
    _leased_query_pools.clear();
    _active_timestamp_lease = -1;
}

void vulkan_command_list::release_queries_on_drop()
{
    // A dropped list never runs, so its pools go back unresolved and every handle keeps its invalid future — which
    // is exactly what "forever not ready" means for a timestamp whose list was dropped.
    for (auto& lease : _leased_query_pools)
        _ctx._query_system.release_pool(cc::move(lease));
    _leased_query_pools.clear();
    _active_timestamp_lease = -1;
}
} // namespace sg::backend::vulkan
