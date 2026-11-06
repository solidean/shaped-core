#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

namespace sg::backend::vulkan
{
class vulkan_context;

} // namespace sg::backend::vulkan

/// The software walk's pool in host-visible memory, which set 3 reads by device address.
class sg::backend::vulkan::vulkan_software_pool final : public sg::impl::software_acceleration_pool
{
public:
    vulkan_software_pool(vulkan_context& ctx);

    /// Frees what the pool holds; the device must be idle.
    void shutdown();

protected:
    [[nodiscard]] cc::result<sg::impl::software_pool_storage> create_storage(isize size_in_bytes) override;
    void retire_storage(sg::impl::software_pool_storage storage) override;

private:
    struct memory
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory device_memory = VK_NULL_HANDLE;
    };
    void destroy(memory m);

    vulkan_context& _vulkan;
    memory _current;
    memory _previous; ///< what create_storage replaced, until retire_storage takes it
};
