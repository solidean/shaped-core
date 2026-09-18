#include "metal_buffer.hh"

#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
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

    // Out of the residency set first, so the set stops naming an allocation that is on its way out.
    if (buffer != nullptr)
        _ctx.residency().remove(buffer);

    // The release comes before the finalizers inside the deferred callback: a finalizer reclaiming the memory a placed
    // resource sits on must never observe a live MTLBuffer still pointing into it.
    _ctx.epochs().defer(
        [buffer, finalizers = cc::move(_finalizers)]() mutable
        {
            if (buffer != nullptr)
                buffer->release();
            for (auto& f : finalizers)
                f();
        });
}
} // namespace sg::backend::metal
