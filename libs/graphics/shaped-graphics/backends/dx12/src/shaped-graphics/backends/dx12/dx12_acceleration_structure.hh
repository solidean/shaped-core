#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/clusters.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>

/// DirectX 12 bottom-level acceleration structure.
/// DXR names a structure by the GPU virtual address of the buffer the driver built it into, so the buffer *is* the
/// structure here — which is a D3D12 fact rather than a portable one, and why the base holds no handle to it.
/// Thin otherwise: the abstract base owns the stats and the expiry protocol.
class sg::backend::dx12::dx12_blas final : public sg::blas
{
public:
    dx12_blas(dx12_buffer_handle storage,
              isize size_in_bytes,
              isize build_scratch_size_in_bytes,
              isize update_scratch_size_in_bytes,
              sg::accel_build_flags build_flags,
              int geometry_count)
      : sg::blas(size_in_bytes, build_scratch_size_in_bytes, update_scratch_size_in_bytes, build_flags, geometry_count),
        _dx12_storage(cc::move(storage))
    {
    }

    /// The storage buffer — its GPU virtual address is the acceleration structure location.
    dx12_buffer_handle _dx12_storage;

private:
    void on_expired() const override
    {
        if (_dx12_storage)
            _dx12_storage->expire();
    }
};

/// DirectX 12 opacity micromap array, the same shape as dx12_blas: DXR names it by its buffer's address too.
class sg::backend::dx12::dx12_micromap final : public sg::micromap
{
public:
    dx12_micromap(dx12_buffer_handle storage,
                  isize size_in_bytes,
                  isize build_scratch_size_in_bytes,
                  cc::vector<sg::micromap_usage> usage)
      : sg::micromap(size_in_bytes, build_scratch_size_in_bytes, cc::move(usage)), _dx12_storage(cc::move(storage))
    {
    }

    dx12_buffer_handle _dx12_storage;

private:
    void on_expired() const override
    {
        if (_dx12_storage)
            _dx12_storage->expire();
    }
};

/// DirectX 12 top-level acceleration structure, the same shape as dx12_blas.
/// The base retains the referenced BLAS handles, which is the ownership edge.
class sg::backend::dx12::dx12_tlas final : public sg::tlas
{
public:
    dx12_tlas(dx12_buffer_handle storage,
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
        _dx12_storage(cc::move(storage))
    {
    }

    dx12_buffer_handle _dx12_storage;

private:
    void on_expired() const override
    {
        if (_dx12_storage)
            _dx12_storage->expire();
    }
};

/// DirectX 12 partitioned tlas, which the preview runtime's ExecuteIndirectRTASOperations builds and updates in place.
/// DXR names it by its buffer's address as it names any tlas, so it binds and traces as one.
class sg::backend::dx12::dx12_partitioned_tlas final : public sg::partitioned_tlas
{
public:
    dx12_partitioned_tlas(sg::partitioned_tlas_description const& description,
                          dx12_buffer_handle storage,
                          isize size_in_bytes,
                          isize scratch_size_in_bytes)
      : sg::partitioned_tlas(description, size_in_bytes, scratch_size_in_bytes, scratch_size_in_bytes),
        _dx12_storage(cc::move(storage))
    {
    }

    dx12_buffer_handle _dx12_storage;

private:
    void on_expired() const override
    {
        if (_dx12_storage)
            _dx12_storage->expire();
    }
};

/// The clusters one described build made, in implicit-destination mode: packed into `_dx12_storage` by the device,
/// which writes each one's address into `_addresses`, read by a cluster BLAS build in place.
class sg::backend::dx12::dx12_cluster_set final : public sg::cluster_set
{
public:
    dx12_cluster_set(int cluster_count,
                     int max_geometry_index,
                     isize size_in_bytes,
                     dx12_buffer_handle storage,
                     dx12_buffer_handle addresses)
      : sg::cluster_set(cluster_count, max_geometry_index, size_in_bytes),
        _dx12_storage(cc::move(storage)),
        _addresses(cc::move(addresses))
    {
    }

    dx12_buffer_handle _dx12_storage;
    dx12_buffer_handle _addresses;
};

/// A cluster pool, built in place like a set from records a compute pass wrote.
class sg::backend::dx12::dx12_cluster_pool final : public sg::cluster_pool
{
public:
    dx12_cluster_pool(sg::cluster_pool_description const& description,
                      isize size_in_bytes,
                      isize scratch_size_in_bytes,
                      dx12_buffer_handle storage,
                      dx12_buffer_handle addresses)
      : sg::cluster_pool(description, size_in_bytes),
        _scratch_size_in_bytes(scratch_size_in_bytes),
        _dx12_storage(cc::move(storage)),
        _addresses(cc::move(addresses))
    {
    }

    isize _scratch_size_in_bytes = 0;
    dx12_buffer_handle _dx12_storage;
    dx12_buffer_handle _addresses;
};

namespace sg::backend::dx12
{
/// The buffer either kind of tlas is built into, whose address its descriptor names.
[[nodiscard]] inline dx12_buffer_handle trace_storage_of(sg::tlas const& tlas)
{
    if (auto const* partitioned = dynamic_cast<dx12_partitioned_tlas const*>(&tlas); partitioned != nullptr)
    {
        CC_ASSERT(partitioned->has_been_updated(), "a partitioned tlas is bound before any update built it");
        return partitioned->_dx12_storage;
    }
    auto const* const plain = dynamic_cast<dx12_tlas const*>(&tlas);
    CC_ASSERT(plain != nullptr, "bound acceleration structure is not a dx12 tlas");
    return plain->_dx12_storage;
}
} // namespace sg::backend::dx12
