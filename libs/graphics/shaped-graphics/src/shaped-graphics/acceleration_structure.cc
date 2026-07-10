#include <shaped-graphics/acceleration_structure.hh>
#include <shaped-graphics/raw_buffer.hh>

namespace sg
{
blas::~blas() = default;

blas::blas(raw_buffer_handle storage,
           isize size_in_bytes,
           isize build_scratch_size_in_bytes,
           isize update_scratch_size_in_bytes,
           accel_build_flags build_flags,
           int geometry_count)
  : _storage(cc::move(storage)),
    _size_in_bytes(size_in_bytes),
    _build_scratch_size_in_bytes(build_scratch_size_in_bytes),
    _update_scratch_size_in_bytes(update_scratch_size_in_bytes),
    _build_flags(build_flags),
    _geometry_count(geometry_count)
{
}

void blas::on_expired() const
{
    if (_storage)
        _storage->expire();
}

tlas::~tlas() = default;

tlas::tlas(raw_buffer_handle storage,
           isize size_in_bytes,
           isize build_scratch_size_in_bytes,
           isize update_scratch_size_in_bytes,
           accel_build_flags build_flags,
           int instance_count,
           cc::vector<blas_handle> referenced_blases)
  : _storage(cc::move(storage)),
    _size_in_bytes(size_in_bytes),
    _build_scratch_size_in_bytes(build_scratch_size_in_bytes),
    _update_scratch_size_in_bytes(update_scratch_size_in_bytes),
    _build_flags(build_flags),
    _instance_count(instance_count),
    _referenced_blases(cc::move(referenced_blases))
{
}

void tlas::on_expired() const
{
    if (_storage)
        _storage->expire();
}
} // namespace sg
