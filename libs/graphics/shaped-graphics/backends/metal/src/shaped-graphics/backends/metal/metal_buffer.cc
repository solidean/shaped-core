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
    if (_buffer == nullptr)
        return;

    // Handed to the epoch rather than released here: a submitted command buffer may still be reading this, and the
    // epoch is what knows when it is not.
    auto* const buffer = _buffer;
    _buffer = nullptr;

    // Out of the residency set first, so the set stops naming an allocation that is on its way out.
    _ctx.residency().remove(buffer);
    _ctx.epochs().defer([buffer] { buffer->release(); });
}
} // namespace sg::backend::metal
