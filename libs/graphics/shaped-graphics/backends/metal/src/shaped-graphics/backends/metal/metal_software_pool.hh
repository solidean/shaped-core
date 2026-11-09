#pragma once

#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

/// The software walk's pool in a shared buffer, which an inline trace of an opted-in package reads at its address.
/// Resident from creation, and released once no recorded work can read it.
class sg::backend::metal::metal_software_pool final : public sg::impl::software_acceleration_pool
{
public:
    explicit metal_software_pool(metal_context& ctx);

    /// Frees what the pool holds; the device must be idle.
    void shutdown();

protected:
    [[nodiscard]] cc::result<sg::impl::software_pool_storage> create_storage(isize size_in_bytes) override;
    void retire_storage(sg::impl::software_pool_storage storage) override;

private:
    metal_context& _metal;
    MTL::Buffer* _current = nullptr;
    MTL::Buffer* _previous = nullptr; ///< what create_storage replaced, until retire_storage takes it
};
