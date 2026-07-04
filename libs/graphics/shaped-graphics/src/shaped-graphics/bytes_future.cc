#include <shaped-graphics/bytes_future.hh>

namespace sg
{
bytes_waiter::~bytes_waiter() = default;

cc::optional<cc::pinned_data<cc::byte const>> bytes_future::try_get_bytes() const
{
    if (_waiter == nullptr || !_waiter->poll_ready())
        return {};
    return _data;
}

cc::optional<cc::pinned_data<cc::byte const>> bytes_future::wait_get_bytes() const
{
    if (_waiter == nullptr || !_waiter->wait())
        return {};
    return _data;
}
} // namespace sg
