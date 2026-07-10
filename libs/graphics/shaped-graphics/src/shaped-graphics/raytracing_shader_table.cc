#include <shaped-graphics/raytracing_shader_table.hh>

namespace sg
{
raytracing_shader_table::~raytracing_shader_table() = default;

raygen_index raytracing_shader_table_description::add_raygen_shader(raygen_shader_handle handle)
{
    auto const index = raygen_index(cc::u32(raygen.size()));
    raygen.push_back(handle);
    return index;
}

miss_index raytracing_shader_table_description::add_miss_shader(miss_shader_handle handle)
{
    auto const index = miss_index(cc::u32(miss.size()));
    miss.push_back(handle);
    return index;
}

hit_index raytracing_shader_table_description::add_hit_shader(hit_shader_handle handle)
{
    auto const index = hit_index(cc::u32(hit.size()));
    hit.push_back(handle);
    return index;
}

callable_index raytracing_shader_table_description::add_callable_shader(callable_shader_handle handle)
{
    auto const index = callable_index(cc::u32(callable.size()));
    callable.push_back(handle);
    return index;
}
} // namespace sg
