#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

/// Vulkan acceleration structures.
///
/// The one shape difference from dx12: a built structure is a **VkAccelerationStructureKHR object placed over the
/// storage buffer**, not just a range of it.
/// DXR names an acceleration structure by the GPU address of its storage, so dx12's subclasses hold nothing but a
/// typed handle to that buffer; here the object is created explicitly and is what a build and a trace refer to.
///
/// It owns no memory of its own — the storage buffer does — so the object is destroyed when this is, and the buffer
/// follows the ordinary expiry path the base already runs.

class sg::backend::vulkan::vulkan_blas final : public sg::blas
{
public:
    vulkan_blas(vulkan_context& ctx,
                vulkan_buffer_handle storage,
                VkAccelerationStructureKHR accel,
                VkDeviceAddress address,
                isize size_in_bytes,
                isize build_scratch_size_in_bytes,
                isize update_scratch_size_in_bytes,
                sg::accel_build_flags build_flags,
                int geometry_count)
      : sg::blas(storage,
                 size_in_bytes,
                 build_scratch_size_in_bytes,
                 update_scratch_size_in_bytes,
                 build_flags,
                 geometry_count),
        _ctx(ctx),
        _vulkan_storage(cc::move(storage)),
        _accel(accel),
        _address(address)
    {
    }

    ~vulkan_blas() override;

    vulkan_context& _ctx;
    vulkan_buffer_handle _vulkan_storage;

    /// The structure object, and the device address a TLAS instance names it by.
    VkAccelerationStructureKHR _accel = VK_NULL_HANDLE;
    VkDeviceAddress _address = 0;
};

class sg::backend::vulkan::vulkan_tlas final : public sg::tlas
{
public:
    vulkan_tlas(vulkan_context& ctx,
                vulkan_buffer_handle storage,
                VkAccelerationStructureKHR accel,
                VkDeviceAddress address,
                isize size_in_bytes,
                isize build_scratch_size_in_bytes,
                isize update_scratch_size_in_bytes,
                sg::accel_build_flags build_flags,
                int instance_count,
                cc::vector<sg::blas_handle> referenced_blases)
      : sg::tlas(storage,
                 size_in_bytes,
                 build_scratch_size_in_bytes,
                 update_scratch_size_in_bytes,
                 build_flags,
                 instance_count,
                 cc::move(referenced_blases)),
        _ctx(ctx),
        _vulkan_storage(cc::move(storage)),
        _accel(accel),
        _address(address)
    {
    }

    ~vulkan_tlas() override;

    vulkan_context& _ctx;
    vulkan_buffer_handle _vulkan_storage;
    VkAccelerationStructureKHR _accel = VK_NULL_HANDLE;
    VkDeviceAddress _address = 0;
};
