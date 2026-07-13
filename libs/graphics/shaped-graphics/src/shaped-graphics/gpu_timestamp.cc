#include <clean-core/common/assert.hh>
#include <shaped-graphics/gpu_timestamp.hh>

namespace sg
{
bool gpu_timestamp::is_ready() const
{
    return _heap_future != nullptr && _heap_future->is_ready();
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
