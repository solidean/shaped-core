#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/acceleration_structure.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>

namespace sg::backend::dx12
{
/// DirectX 12 bottom-level acceleration structure. The built structure lives in the storage buffer the
/// base holds; this subclass keeps a typed handle to it so a TLAS build can read its GPU virtual address
/// (the AS location) without re-casting. Thin — the abstract base owns the lifetime + stats.
class dx12_blas final : public sg::blas
{
public:
    dx12_blas(dx12_buffer_handle storage,
              cc::isize size_in_bytes,
              cc::isize build_scratch_size_in_bytes,
              cc::isize update_scratch_size_in_bytes,
              sg::accel_build_flags build_flags,
              int geometry_count)
      : sg::blas(storage,
                 size_in_bytes,
                 build_scratch_size_in_bytes,
                 update_scratch_size_in_bytes,
                 build_flags,
                 geometry_count),
        _dx12_storage(cc::move(storage))
    {
    }

    /// The storage buffer as a dx12_buffer — its GPU virtual address is the acceleration structure location.
    dx12_buffer_handle _dx12_storage;
};

/// DirectX 12 top-level acceleration structure. Same shape as dx12_blas; the base retains the referenced
/// BLAS handles (the ownership edge).
class dx12_tlas final : public sg::tlas
{
public:
    dx12_tlas(dx12_buffer_handle storage,
              cc::isize size_in_bytes,
              cc::isize build_scratch_size_in_bytes,
              cc::isize update_scratch_size_in_bytes,
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
        _dx12_storage(cc::move(storage))
    {
    }

    dx12_buffer_handle _dx12_storage;
};
} // namespace sg::backend::dx12
