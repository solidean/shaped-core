#include <shaped-graphics/transfer/stream_handle.hh>

namespace sg::impl
{
void stream_handle_base::promote_to_async()
{
    if (_control == nullptr)
        return;

    // Exchange rather than a plain store: the backend half stamps a resource, and doing that twice is at best
    // wasted work and at worst a second reservation nothing will ever signal.
    if (_control->promoted.exchange(true, std::memory_order_acq_rel))
        return;
    if (_control->on_promote)
        _control->on_promote();
}
} // namespace sg::impl
