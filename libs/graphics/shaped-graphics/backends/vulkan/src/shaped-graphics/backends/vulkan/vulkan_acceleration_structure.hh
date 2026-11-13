#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/clusters.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>

/// Vulkan acceleration structures.
///
/// The one shape difference from dx12: a built structure is a **VkAccelerationStructureKHR object placed over the
/// storage buffer**, not just a range of it.
/// DXR names an acceleration structure by the GPU address of its storage, so dx12's subclasses hold nothing but a
/// typed handle to that buffer; here the object is created explicitly and is what a build and a trace refer to.
///
/// It owns no memory of its own — the storage buffer does — so the object is destroyed when this is, and expiry
/// releases the buffer under it.

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
      : sg::blas(size_in_bytes, build_scratch_size_in_bytes, update_scratch_size_in_bytes, build_flags, geometry_count),
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

private:
    void on_expired() const override
    {
        if (_vulkan_storage)
            _vulkan_storage->expire();
    }
};

/// A micromap is the same shape: a VkMicromapEXT object placed over its storage buffer.
class sg::backend::vulkan::vulkan_micromap final : public sg::micromap
{
public:
    vulkan_micromap(vulkan_context& ctx,
                    vulkan_buffer_handle storage,
                    VkMicromapEXT micromap,
                    isize size_in_bytes,
                    isize build_scratch_size_in_bytes,
                    cc::vector<sg::micromap_usage> usage)
      : sg::micromap(size_in_bytes, build_scratch_size_in_bytes, cc::move(usage)),
        _ctx(ctx),
        _vulkan_storage(cc::move(storage)),
        _micromap(micromap)
    {
    }

    ~vulkan_micromap() override;

    vulkan_context& _ctx;
    vulkan_buffer_handle _vulkan_storage;
    VkMicromapEXT _micromap = VK_NULL_HANDLE;

private:
    void on_expired() const override
    {
        if (_vulkan_storage)
            _vulkan_storage->expire();
    }
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
      : sg::tlas(size_in_bytes,
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

private:
    void on_expired() const override
    {
        if (_vulkan_storage)
            _vulkan_storage->expire();
    }
};

/// A partitioned tlas: a buffer and its device address, which `VK_NV_partitioned_acceleration_structure` builds and
/// updates in place, with no `VkAccelerationStructureKHR` of its own.
/// So no descriptor can name it, and SGL's own SPIR-V traces it by its address (`sg_acceleration_addresses`).
class sg::backend::vulkan::vulkan_partitioned_tlas final : public sg::partitioned_tlas
{
public:
    vulkan_partitioned_tlas(sg::partitioned_tlas_description const& description,
                            vulkan_buffer_handle storage,
                            VkDeviceAddress address,
                            isize size_in_bytes,
                            isize build_scratch_size_in_bytes,
                            isize update_scratch_size_in_bytes)
      : sg::partitioned_tlas(description, size_in_bytes, build_scratch_size_in_bytes, update_scratch_size_in_bytes),
        _vulkan_storage(cc::move(storage)),
        _address(address)
    {
    }

    vulkan_buffer_handle _vulkan_storage;
    VkDeviceAddress _address = 0;

private:
    void on_expired() const override
    {
        if (_vulkan_storage)
            _vulkan_storage->expire();
    }
};

/// The clusters one build made, in implicit-destination mode: the driver packs them into `_vulkan_storage` and writes
/// each one's address into `_addresses`, which a cluster BLAS build reads by range, so the CPU never learns them.
class sg::backend::vulkan::vulkan_cluster_set final : public sg::cluster_set
{
public:
    vulkan_cluster_set(int cluster_count,
                       int max_geometry_index,
                       isize size_in_bytes,
                       vulkan_buffer_handle storage,
                       vulkan_buffer_handle addresses)
      : sg::cluster_set(cluster_count, max_geometry_index, size_in_bytes),
        _vulkan_storage(cc::move(storage)),
        _addresses(cc::move(addresses))
    {
    }

    vulkan_buffer_handle _vulkan_storage;
    /// One `VkDeviceAddress` per cluster, in build order.
    vulkan_buffer_handle _addresses;
};

/// A cluster pool, built in place in implicit-destination mode like a set, from records a compute pass wrote.
class sg::backend::vulkan::vulkan_cluster_pool final : public sg::cluster_pool
{
public:
    vulkan_cluster_pool(sg::cluster_pool_description const& description,
                        isize size_in_bytes,
                        isize build_scratch_size_in_bytes,
                        vulkan_buffer_handle storage,
                        vulkan_buffer_handle addresses)
      : sg::cluster_pool(description, size_in_bytes),
        _build_scratch_size_in_bytes(build_scratch_size_in_bytes),
        _vulkan_storage(cc::move(storage)),
        _addresses(cc::move(addresses))
    {
    }

    isize _build_scratch_size_in_bytes = 0;
    vulkan_buffer_handle _vulkan_storage;
    /// One `VkDeviceAddress` per cluster of the last build, in record order.
    vulkan_buffer_handle _addresses;
};

/// The storage and the address array of a cluster set or a pool, which a cluster BLAS build reads.
struct sg::backend::vulkan::vulkan_clusters
{
    vulkan_buffer_handle storage;
    vulkan_buffer_handle addresses;
};

/// What a binding takes of a tlas, either kind: the storage a trace reads, the address a trace converts, and what its
/// descriptor names, which is 0, the null structure, for a partitioned one.
struct sg::backend::vulkan::vulkan_trace_target
{
    vulkan_buffer_handle storage;
    VkDeviceAddress address = 0;
    VkDeviceAddress descriptor_address = 0;
};

namespace sg::backend::vulkan
{

[[nodiscard]] inline vulkan_trace_target trace_target_of(sg::tlas const& tlas)
{
    if (auto const* partitioned = dynamic_cast<vulkan_partitioned_tlas const*>(&tlas); partitioned != nullptr)
    {
        CC_ASSERT(partitioned->has_been_updated(), "a partitioned tlas is bound before any update built it");
        return {.storage = partitioned->_vulkan_storage, .address = partitioned->_address, .descriptor_address = 0};
    }
    auto const& plain = static_cast<vulkan_tlas const&>(tlas);
    return {.storage = plain._vulkan_storage, .address = plain._address, .descriptor_address = plain._address};
}
} // namespace sg::backend::vulkan
