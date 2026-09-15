#include <shaped-graphics/raytracing/acceleration_structure.hh>

namespace sg
{
blas::~blas() = default;

blas::blas(isize size_in_bytes,
           isize build_scratch_size_in_bytes,
           isize update_scratch_size_in_bytes,
           accel_build_flags build_flags,
           int geometry_count)
  : _size_in_bytes(size_in_bytes),
    _build_scratch_size_in_bytes(build_scratch_size_in_bytes),
    _update_scratch_size_in_bytes(update_scratch_size_in_bytes),
    _build_flags(build_flags),
    _geometry_count(geometry_count)
{
}

tlas::~tlas() = default;

tlas::tlas(isize size_in_bytes,
           isize build_scratch_size_in_bytes,
           isize update_scratch_size_in_bytes,
           accel_build_flags build_flags,
           int instance_count,
           cc::vector<blas_handle> referenced_blases)
  : _size_in_bytes(size_in_bytes),
    _build_scratch_size_in_bytes(build_scratch_size_in_bytes),
    _update_scratch_size_in_bytes(update_scratch_size_in_bytes),
    _build_flags(build_flags),
    _instance_count(instance_count),
    _referenced_blases(cc::move(referenced_blases))
{
}
} // namespace sg
