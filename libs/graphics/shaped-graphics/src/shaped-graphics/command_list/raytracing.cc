#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/impl/hit_record_check.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

namespace sg
{
void command_list::bind_raytracing_pipeline(raytracing_pipeline const& pipeline)
{
    if (auto const* layout = pipeline.footprint().layout(); layout == nullptr || layout != _raytracing_layout)
    {
        _raytracing_layout = layout;
        for (auto& g : _raytracing_groups)
            g = nullptr;
    }
    raytracing_bind_pipeline(pipeline);
}

void command_list::bind_raytracing_group(int group_index, binding_group const& group)
{
    CC_ASSERT(group_index >= 0 && group_index < max_binding_groups, "binding-group slot out of range");
    _raytracing_groups[group_index] = &group;
    raytracing_bind_group(group_index, group);
}

void command_list::dispatch_rays(raytracing_shader_table const& table, raygen_index raygen, int width, int height, int depth)
{
    if (context().hit_record_checks() && check_hit_records(table))
    {
        CC_LOG_ERROR("dispatch_rays skipped a dispatch whose hit records reach past the shader table's end");
        return;
    }
    raytracing_dispatch_rays(table, raygen, width, height, depth);
}

bool command_list::check_hit_records(raytracing_shader_table const& table)
{
    auto past_table_end = false;
    for (auto const* group : _raytracing_groups)
    {
        if (group == nullptr)
            continue;
        for (auto const& tlas : impl::tlases_of(*group))
        {
            auto const* checked = static_cast<checked_trace const*>(nullptr);
            for (auto const& c : _checked_traces)
                if (c.table.get() == &table && c.tlas == tlas)
                    checked = &c;
            if (checked != nullptr)
            {
                past_table_end = past_table_end || checked->past_table_end;
                continue;
            }

            auto const mismatches = impl::find_hit_record_mismatches(table, *tlas);
            auto text = cc::string();
            auto reaches_past_end = false;
            for (auto const& m : mismatches)
            {
                text += cc::format("\n  {}", m.message);
                reaches_past_end = reaches_past_end || m.past_table_end;
            }
            // A table no handle owns keys nothing, so it is checked at every dispatch rather than skipped wrongly.
            _checked_traces.push_back(
                {.table = table.weak_from_this().lock(), .tlas = tlas, .past_table_end = reaches_past_end});
            past_table_end = past_table_end || reaches_past_end;
            // Logged rather than asserted: the hit groups come from shaders that hot reload can change under a running program.
            if (!mismatches.empty())
                CC_LOG_ERROR("dispatch_rays traces a tlas whose instances reach hit records of the wrong kind or out "
                             "of "
                             "the shader table, which is undefined behavior:{}",
                             text);
        }
    }
    return past_table_end;
}

bool command_list_raytracing_scope::is_supported() const
{
    return _cmd.raytracing_is_supported();
}

blas_handle command_list_raytracing_scope::build_blas(cc::span<blas_triangles const> geometries,
                                                      accel_build_flags flags,
                                                      int hit_record_stride)
{
    CC_ASSERT(hit_record_stride >= 1, "build_blas: hit_record_stride must be >= 1");
    _cmd._stats.add(stat::blas_builds);
    auto blas = _cmd.raytracing_build_blas_triangles(geometries, flags, hit_record_stride);
    if (blas != nullptr)
        impl::set_build_record(*blas, blas_geometry::triangles, hit_record_stride);
    return blas;
}

blas_handle command_list_raytracing_scope::build_blas(cc::span<blas_aabbs const> geometries,
                                                      accel_build_flags flags,
                                                      int hit_record_stride)
{
    CC_ASSERT(hit_record_stride >= 1, "build_blas: hit_record_stride must be >= 1");
    _cmd._stats.add(stat::blas_builds);
    auto blas = _cmd.raytracing_build_blas_aabbs(geometries, flags, hit_record_stride);
    if (blas != nullptr)
        impl::set_build_record(*blas, blas_geometry::aabbs, hit_record_stride);
    return blas;
}

tlas_handle command_list_raytracing_scope::build_tlas(cc::span<tlas_instance const> instances, accel_build_flags flags)
{
    _cmd._stats.add(stat::tlas_builds);
    auto tlas = _cmd.raytracing_build_tlas(instances, flags);

    // Only what the hit-record check at dispatch_rays reads, and only where it runs.
    if (tlas != nullptr && _cmd.context().hit_record_checks())
    {
        auto records = cc::vector<impl::tlas_instance_record>();
        records.reserve(instances.size());
        for (auto const& inst : instances)
            records.push_back({.blas = inst.blas, .hit_group_offset = inst.hit_group_offset, .mask = inst.mask});
        impl::set_instance_records(*tlas, cc::move(records));
    }
    return tlas;
}

void command_list_raytracing_scope::bind_pipeline(raytracing_pipeline const& pipeline)
{
    _cmd.bind_raytracing_pipeline(pipeline);
}

void command_list_raytracing_scope::bind_group(int group_index, binding_group const& group)
{
    _cmd.bind_raytracing_group(group_index, group);
}

void command_list_raytracing_scope::dispatch_rays(raytracing_shader_table const& table,
                                                  raygen_index raygen,
                                                  int width,
                                                  int height,
                                                  int depth)
{
    _cmd._stats.add(stat::ray_dispatches);
    _cmd.dispatch_rays(table, raygen, width, height, depth);
}

array_access_declaration command_list_raytracing_scope::declare_array_buffer_access(
    buffer_array_binding array,
    cc::span<array_buffer_access const> elements)
{
    // Ray tracing binds through the compute path (same root signature, same bind point), so the compute seam serves both.
    return _cmd.adopt_array_declaration(_cmd.compute_declare_array_buffer_access(array.name, elements), false);
}

array_access_declaration command_list_raytracing_scope::declare_array_buffer_access(
    cc::string_view binding_name,
    cc::span<array_buffer_access const> elements)
{
    return declare_array_buffer_access(_cmd.named_buffer_array(binding_name), elements);
}

array_access_declaration command_list_raytracing_scope::declare_array_texture_access(
    texture_array_binding array,
    cc::span<array_texture_access const> elements)
{
    return _cmd.adopt_array_declaration(_cmd.compute_declare_array_texture_access(array.name, elements), false);
}

array_access_declaration command_list_raytracing_scope::declare_array_texture_access(
    cc::string_view binding_name,
    cc::span<array_texture_access const> elements)
{
    return declare_array_texture_access(_cmd.named_texture_array(binding_name), elements);
}
} // namespace sg
