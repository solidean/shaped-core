#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>

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

void command_list_raytracing_scope::bind_pipeline(raytracing_pipeline const& pipeline)
{
    _cmd.raytracing_bind_pipeline(pipeline);
}

void command_list_raytracing_scope::bind_group(int set, binding_group const& group)
{
    _cmd.raytracing_bind_group(set, group);
}

void command_list_raytracing_scope::dispatch_rays(raytracing_shader_table const& table,
                                                  raygen_index raygen,
                                                  int width,
                                                  int height,
                                                  int depth)
{
    _cmd.raytracing_dispatch_rays(table, raygen, width, height, depth);
}

void command_list_raytracing_scope::declare_array_buffer_access(cc::string_view binding_name,
                                                                cc::span<array_buffer_access const> elements)
{
    // Ray tracing binds through the compute path (same root signature, same pending state), so the compute seam serves both.
    _cmd.compute_declare_array_buffer_access(binding_name, elements);
}

void command_list_raytracing_scope::declare_array_texture_access(cc::string_view binding_name,
                                                                 cc::span<array_texture_access const> elements)
{
    _cmd.compute_declare_array_texture_access(binding_name, elements);
}
} // namespace sg
