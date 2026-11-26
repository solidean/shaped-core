#include "metal_buffer.hh"

#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
sg::context& ctx_base_of(metal_context& ctx)
{
    return ctx;
}

metal_buffer::~metal_buffer()
{
    release_storage();
} // no-op if expire() already released the storage

void metal_buffer::on_expired() const
{
    release_storage();
}

void metal_buffer::release_storage() const
{
    // An empty buffer owns no MTLBuffer, and a second call owns nothing either — but either may still carry finalizers,
    // which are the caller's feedback point and must run exactly once.
    if (_buffer == nullptr && _finalizers.empty())
        return;

    // Handed to the epoch rather than released here: a submitted command buffer may still be reading this, and the
    // epoch is what knows when it is not.
    auto* const buffer = _buffer;
    _buffer = nullptr;

    // The release comes before the finalizers inside the deferred callback: a finalizer reclaiming the memory a placed
    // resource sits on must never observe a live MTLBuffer still pointing into it.
    // The residency entry goes with the release, never before — see metal_residency_set.
    _ctx.epochs().defer(
        [buffer, &residency = _ctx.residency(), finalizers = cc::move(_finalizers)]() mutable
        {
            if (buffer != nullptr)
            {
                residency.remove(buffer);
                buffer->release();
            }
            for (auto& f : finalizers)
                f();
        });
}
} // namespace sg::backend::metal
