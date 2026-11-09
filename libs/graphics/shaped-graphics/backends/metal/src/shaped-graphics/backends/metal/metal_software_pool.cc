#include "metal_software_pool.hh"

#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
metal_software_pool::metal_software_pool(metal_context& ctx) : sg::impl::software_acceleration_pool(ctx), _metal(ctx)
{
}

cc::result<sg::impl::software_pool_storage> metal_software_pool::create_storage(isize size_in_bytes)
{
    auto* const buffer = _metal.device()->newBuffer(
        NS::UInteger(size_in_bytes), MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    if (buffer == nullptr)
        return cc::error(cc::format("the metal device refused a {} byte software acceleration pool", size_in_bytes));
    buffer->setLabel(ns_string("sg software acceleration pool"));
    cc::memset(buffer->contents(), 0, size_t(size_in_bytes));
    _metal.residency().add(buffer);
    _previous = _current;
    _current = buffer;
    return sg::impl::software_pool_storage{
        .mapped = static_cast<byte*>(buffer->contents()),
        .gpu_address = u64(buffer->gpuAddress()),
        .size_in_bytes = size_in_bytes,
    };
}

void metal_software_pool::retire_storage(sg::impl::software_pool_storage)
{
    // the base retires the storage create_storage replaced, right after copying it
    auto* const previous = _previous;
    _previous = nullptr;
    if (previous == nullptr)
        return;
    _metal.residency().remove(previous);
    _metal.epochs().defer([previous] { previous->release(); });
}

void metal_software_pool::shutdown()
{
    for (auto** const owned : {&_previous, &_current})
        if (*owned != nullptr)
        {
            _metal.residency().remove(*owned);
            (*owned)->release();
            *owned = nullptr;
        }
}
} // namespace sg::backend::metal
