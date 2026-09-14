#include <clean-core/common/assert.hh>
#include <shaped-graphics/query/gpu_timestamp.hh>

namespace sg
{
bool gpu_timestamp::is_ready() const
{
    return _heap_future != nullptr && _heap_future->is_ready();
}

cc::shared_async<cc::unit const> gpu_timestamp::completion() const
{
    return _heap_future != nullptr ? _heap_future->completion() : cc::shared_async<cc::unit const>();
}

cc::shared_async<u64> gpu_timestamp::ticks() const
{
    if (_heap_future == nullptr)
        return {};
    return cc::make_async_lazy(
        [index = _index](cc::pinned_data<u64 const> const& heap)
        {
            CC_ASSERT(index < heap.size(), "timestamp index out of range for its heap download");
            return heap[index];
        },
        _heap_future->data());
}

cc::optional<u64> gpu_timestamp::try_get_ticks() const
{
    if (_heap_future == nullptr)
        return {};
    auto const data = _heap_future->try_get_data();
    if (!data.has_value())
        return {};
    CC_ASSERT(_index < data.value().size(), "timestamp index out of range for its heap download");
    return data.value()[_index];
}

cc::optional<double> gpu_timestamp::try_get_seconds() const
{
    auto const ticks = try_get_ticks();
    if (!ticks.has_value())
        return {};
    return double(ticks.value()) * _tick_to_seconds;
}
} // namespace sg
