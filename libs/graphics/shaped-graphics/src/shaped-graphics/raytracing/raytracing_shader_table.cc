#include <clean-core/common/assert.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

namespace sg
{
raytracing_shader_table::raytracing_shader_table(raytracing_shader_table_description const& desc)
  : _pipeline(desc.pipeline), _hit_records(desc.hit), _ray_count(desc.ray_count)
{
    CC_ASSERT(desc.ray_count >= 1, "a shader table's ray_count must be >= 1");
}

raytracing_shader_table::~raytracing_shader_table() = default;

u32 raytracing_shader_table::offset_of(hit_row row) const
{
    CC_ASSERT(isize(u32(row)) + isize(_ray_count) <= _hit_records.size(), "this hit row is not one of this table's");
    return u32(row);
}

raygen_index raytracing_shader_table_description::add_raygen_shader(raygen_shader_handle handle)
{
    auto const index = raygen_index(u32(raygen.size()));
    raygen.push_back(handle);
    return index;
}

miss_index raytracing_shader_table_description::add_miss_shader(miss_shader_handle handle)
{
    auto const index = miss_index(u32(miss.size()));
    miss.push_back(handle);
    return index;
}

hit_index raytracing_shader_table_description::add_hit_shader(hit_shader_handle handle)
{
    auto const index = hit_index(u32(hit.size()));
    hit.push_back(handle);
    return index;
}

hit_row raytracing_shader_table_description::add_hit_row(cc::span<hit_shader_handle const> per_ray_type)
{
    CC_ASSERT(ray_count >= 1, "a shader table's ray_count must be >= 1");
    CC_ASSERT(per_ray_type.size() == isize(ray_count), "a hit row holds exactly one record per ray type");
    auto const row = hit_row(u32(hit.size()));
    for (auto const handle : per_ray_type)
        hit.push_back(handle);
    return row;
}

callable_index raytracing_shader_table_description::add_callable_shader(callable_shader_handle handle)
{
    auto const index = callable_index(u32(callable.size()));
    callable.push_back(handle);
    return index;
}
} // namespace sg
