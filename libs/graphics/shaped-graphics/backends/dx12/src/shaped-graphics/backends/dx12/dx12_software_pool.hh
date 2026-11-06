#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

namespace sg::backend::dx12
{
class dx12_context;

} // namespace sg::backend::dx12

/// The software walk's pool in an upload heap, which a root SRV reads directly.
class sg::backend::dx12::dx12_software_pool final : public sg::impl::software_acceleration_pool
{
public:
    dx12_software_pool(dx12_context& ctx);

    /// Frees what the pool holds; the device must be idle.
    void shutdown();

protected:
    [[nodiscard]] cc::result<sg::impl::software_pool_storage> create_storage(isize size_in_bytes) override;
    void retire_storage(sg::impl::software_pool_storage storage) override;

private:
    dx12_context& _dx12;
    ComPtr<ID3D12Resource> _current;
    ComPtr<ID3D12Resource> _previous; ///< what create_storage replaced, until retire_storage takes it
};
