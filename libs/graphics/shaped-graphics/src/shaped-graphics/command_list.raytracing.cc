#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.raytracing.hh>

namespace sg
{
bool command_list_raytracing_scope::is_supported() const
{
    return _cmd.raytracing_is_supported();
}

blas_handle command_list_raytracing_scope::build_blas(cc::span<blas_triangles const> geometries, accel_build_flags flags)
{
    return _cmd.raytracing_build_blas_triangles(geometries, flags);
}

blas_handle command_list_raytracing_scope::build_blas(cc::span<blas_aabbs const> geometries, accel_build_flags flags)
{
    return _cmd.raytracing_build_blas_aabbs(geometries, flags);
}

tlas_handle command_list_raytracing_scope::build_tlas(cc::span<tlas_instance const> instances, accel_build_flags flags)
{
    return _cmd.raytracing_build_tlas(instances, flags);
}
} // namespace sg
