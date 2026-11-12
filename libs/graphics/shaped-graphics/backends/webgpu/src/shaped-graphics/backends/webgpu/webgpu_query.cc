// Timestamp queries: the query set pool, and cmd.query on the command list.

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>

namespace sg::backend::webgpu
{
void webgpu_query_system::initialize(webgpu_context& ctx, bool supported)
{
    _ctx = &ctx;
    _supported = supported;
}

void webgpu_query_system::shutdown()
{
    _free = {};
    _leases = {};
}

webgpu_query_lease* webgpu_query_system::lease()
{
    CC_ASSERT(_supported, "timestamp queries are not supported on this device");
    if (!_free.empty())
    {
        auto* const lease = _free.pop_back();
        lease->used = 0;
        lease->future = std::make_shared<sg::data_future<u64>>();
        return lease;
    }

    auto lease = std::make_unique<webgpu_query_lease>();
    auto const set_desc = WGPUQuerySetDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg timestamps"),
        .type = WGPUQueryType_Timestamp,
        .count = u32(queries_per_set),
    };
    lease->query_set = wgpu_query_set(wgpuDeviceCreateQuerySet(_ctx->device(), &set_desc));
    auto const resolve_desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg timestamp resolve"),
        .usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc,
        .size = u64(queries_per_set) * sizeof(u64),
        .mappedAtCreation = WGPU_FALSE,
    };
    lease->resolve = wgpu_buffer(wgpuDeviceCreateBuffer(_ctx->device(), &resolve_desc));
    lease->future = std::make_shared<sg::data_future<u64>>();
    auto* const raw = lease.get();
    _leases.push_back(cc::move(lease));
    return raw;
}

void webgpu_query_system::give_back(webgpu_query_lease* lease)
{
    _free.push_back(lease);
}

bool webgpu_command_list::query_timestamps_supported() const
{
    return _ctx._queries.is_supported();
}

sg::gpu_timestamp webgpu_command_list::query_record_gpu_timestamp()
{
    if (!_ctx._queries.is_supported())
        return {};
    _is_timed = true;

    // The end of the pass just recorded, where nothing ran since: the pass's own end waits for its work.
    end_open_pass("a timestamp query");
    if (_last_pass_end.index >= 0)
    {
        auto const slot = _last_pass_end;
        _last_pass_end = {};
        return sg::gpu_timestamp(std::shared_ptr<sg::data_future<u64> const>(slot.lease->future), isize(slot.index),
                                 1e-9);
    }

    // Otherwise the beginning of the next pass, or of an empty one where none comes first.
    flush_pending_begin();
    if (_query_leases.empty() || _query_leases.back()->used >= webgpu_query_system::usable_queries_per_set)
        _query_leases.push_back(_ctx._queries.lease());
    auto* const lease = _query_leases.back();
    _pending_begin = {.lease = lease, .index = lease->used++};
    // Nanoseconds.
    return sg::gpu_timestamp(std::shared_ptr<sg::data_future<u64> const>(lease->future), isize(_pending_begin.index),
                             1e-9);
}

WGPUPassTimestampWrites const* webgpu_command_list::pass_timestamp_writes(WGPUPassTimestampWrites& storage)
{
    if (!_is_timed)
        return nullptr;
    // WORKAROUND: a pass that writes one end only names the set's last slot for the other, which nothing reads.
    // emdawnwebgpu passes WGPU_QUERY_SET_INDEX_UNDEFINED to JS as 4294967295 rather than as an absent field, which Dawn tolerates and wgpu refuses as out of bounds.
    // See docs/bugs-external/webgpu-timestamp-write-index-sentinel; with the fix this goes back to the undefined index.
    constexpr auto discard = u32(webgpu_query_system::queries_per_set - 1);
    auto* lease = _pending_begin.lease;
    if (lease == nullptr)
    {
        if (_query_leases.empty() || _query_leases.back()->used >= webgpu_query_system::usable_queries_per_set)
            _query_leases.push_back(_ctx._queries.lease());
        lease = _query_leases.back();
    }
    // one set holds both ends, so a full one leaves this pass's end unclaimed
    auto const end = lease->used < webgpu_query_system::usable_queries_per_set ? lease->used++ : -1;
    storage = WGPUPassTimestampWrites{
        .nextInChain = nullptr,
        .querySet = lease->query_set.get(),
        .beginningOfPassWriteIndex = _pending_begin.index >= 0 ? u32(_pending_begin.index) : discard,
        .endOfPassWriteIndex = end >= 0 ? u32(end) : discard,
    };
    _open_pass_end = end >= 0 ? query_slot{.lease = lease, .index = end} : query_slot{};
    _pending_begin = {};
    return &storage;
}

void webgpu_command_list::flush_pending_begin()
{
    if (_pending_begin.index < 0)
        return;
    auto const writes = WGPUPassTimestampWrites{
        .nextInChain = nullptr,
        .querySet = _pending_begin.lease->query_set.get(),
        .beginningOfPassWriteIndex = u32(_pending_begin.index),
        .endOfPassWriteIndex = u32(webgpu_query_system::queries_per_set - 1),
    };
    _pending_begin = {};
    auto const desc
        = WGPUComputePassDescriptor{.nextInChain = nullptr, .label = to_wgpu("sg timestamp"), .timestampWrites = &writes};
    auto pass = wgpu_compute_pass(wgpuCommandEncoderBeginComputePass(_encoder.get(), &desc));
    wgpuComputePassEncoderEnd(pass.get());
}

void webgpu_command_list::finalize_queries()
{
    end_open_pass("a query resolve");
    flush_pending_begin();
    for (auto* const lease : _query_leases)
    {
        auto const bytes = isize(lease->used) * isize(sizeof(u64));
        end_open_pass("a query resolve");
        wgpuCommandEncoderResolveQuerySet(encoder(), lease->query_set.get(), 0, u32(lease->used), lease->resolve.get(),
                                          0);
        auto readback = _ctx._readbacks.acquire(bytes);
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), lease->resolve.get(), 0, readback.staging.get(), 0, u64(bytes));

        auto destination = cc::pinned_data<byte>::create_uninitialized(bytes);
        auto const dst_span = destination.span();
        auto future = record_readback(cc::move(readback), cc::move(destination),
                                      [dst_span](cc::span<byte const> mapped)
                                      {
                                          cc::memcpy(dst_span.data(), mapped.data(), size_t(dst_span.size()));
                                          return true;
                                      });
        *lease->future = sg::data_future<u64>(cc::move(future));
    }
}

void webgpu_command_list::release_queries()
{
    for (auto* const lease : _query_leases)
        _ctx._queries.give_back(lease);
    _query_leases.clear();
}
} // namespace sg::backend::webgpu
