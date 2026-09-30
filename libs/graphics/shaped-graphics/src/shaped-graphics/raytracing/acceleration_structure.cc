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

void impl::set_build_record(blas const& blas, blas_geometry geometry, int hit_record_stride)
{
    // Only ever called on a structure nobody else holds yet, and never on one created const.
    auto& b = const_cast<sg::blas&>(blas);
    b._geometry = geometry;
    b._hit_record_stride = hit_record_stride;
}

void impl::set_instance_records(tlas const& tlas, cc::vector<tlas_instance_record> records)
{
    // Only ever called on a structure nobody else holds yet, and never on one created const.
    const_cast<sg::tlas&>(tlas)._instance_records = cc::move(records);
}

cc::span<impl::tlas_instance_record const> impl::instance_records_of(tlas const& tlas)
{
    return tlas._instance_records;
}
} // namespace sg
