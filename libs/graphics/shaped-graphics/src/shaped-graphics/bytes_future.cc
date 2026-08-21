#include <shaped-graphics/bytes_future.hh>

namespace sg
{
cc::optional<cc::pinned_data<byte const>> bytes_future::try_get_bytes() const
{
    if (!is_ready() || _completion->has_error())
        return {};
    return _data;
}

cc::optional<cc::pinned_data<byte const>> bytes_future::wait_get_bytes() const
{
    if (_completion == nullptr)
        return {};

    // Nothing will ever push this node until the caller submits, so blocking here would stall the thread that must.
    if (!_completion->is_ready() && _gate != nullptr && !_gate->is_waitable())
        return {};

    if (!cc::try_async_blocking_get(_completion).has_value())
        return {}; // cancelled or failed
    return _data;
}
} // namespace sg
