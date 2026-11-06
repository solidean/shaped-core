#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_software_pool.hh>

namespace sg::backend::dx12
{
dx12_software_pool::dx12_software_pool(dx12_context& ctx) : sg::impl::software_acceleration_pool(ctx), _dx12(ctx)
{
}

cc::result<sg::impl::software_pool_storage> dx12_software_pool::create_storage(isize size_in_bytes)
{
    auto buffer = create_mapped_ring_buffer(_dx12._device.Get(), D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, size_in_bytes);
    CC_RETURN_IF_ERROR(buffer);
    auto* const mapped = static_cast<byte*>(buffer.value().mapped);
    cc::memset(mapped, 0, size_t(size_in_bytes));
    _previous = cc::move(_current);
    _current = cc::move(buffer.value().resource);
    return sg::impl::software_pool_storage{
        .mapped = mapped,
        .gpu_address = _current->GetGPUVirtualAddress(),
        .size_in_bytes = size_in_bytes,
    };
}

void dx12_software_pool::retire_storage(sg::impl::software_pool_storage)
{
    // the base retires the storage create_storage replaced, right after copying it
    _dx12.defer_until_retired([keep = cc::move(_previous)] {});
}

void dx12_software_pool::shutdown()
{
    _previous.Reset();
    _current.Reset();
}
} // namespace sg::backend::dx12
