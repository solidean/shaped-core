#include <clean-core/common/assertf.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
#include <shaped-shader-library/shader_asset.hh>

cc::shared_async<sg::raytracing_pipeline_description> slib::describe_raytracing_pipeline(
    sg::context* ctx,
    raytracing_pipeline_definition const* definition,
    cc::vector<sg::hit_shader> host_hit_shaders)
{
    auto const& d = *definition;
    CC_ASSERTF(d.raygen != nullptr && *d.raygen != nullptr,
               "{}'s {}: its package was never added to a shader library, so it has no shaders", d.file, d.name);
    CC_ASSERTF(d.has_host_hit_groups || host_hit_shaders.empty(),
               "{}'s {}: the pipeline lists every hit group, and the host handed it {} more hit shaders", d.file,
               d.name, host_hit_shaders.size());
    CC_ASSERTF(host_hit_shaders.size() % d.ray_count == 0,
               "{}'s {}: a hit group is {} hit shaders, one per ray type, and the host handed {}", d.file, d.name,
               d.ray_count, host_hit_shaders.size());

    auto desc = sg::raytracing_pipeline_description();
    desc.layout = d.acquire_layout(*ctx);
    desc.max_recursion_depth = d.max_recursion_depth;
    desc.max_payload_size = d.max_payload_size;
    desc.max_attribute_size = d.max_attribute_size;

    (void)desc.add_raygen_shader(co_await (*d.raygen)->acquire(*ctx));
    for (auto const* const miss : d.misses)
    {
        CC_ASSERTF(miss != nullptr, "{}'s {}: every ray type has a miss", d.file, d.name);
        (void)desc.add_miss_shader(co_await (*miss)->acquire(*ctx));
    }
    for (auto const& group : d.hit_groups)
    {
        auto intersection = cc::optional<sg::compiled_shader>();
        if (group.intersection != nullptr)
            intersection = co_await (*group.intersection)->acquire(*ctx);
        for (auto r = 0; r < d.ray_count; ++r)
        {
            auto shader = sg::hit_shader{.intersection = intersection};
            if (auto const* const h = group.closest_hits[r]; h != nullptr)
                shader.closest_hit = co_await (*h)->acquire(*ctx);
            if (auto const* const h = group.any_hits[r]; h != nullptr)
                shader.any_hit = co_await (*h)->acquire(*ctx);
            (void)desc.add_hit_shader(cc::move(shader));
        }
    }
    for (auto& shader : host_hit_shaders)
        (void)desc.add_hit_shader(cc::move(shader));
    co_return desc;
}

sg::raytracing_shader_table_description slib::table_description(raytracing_pipeline_definition const& definition,
                                                                sg::raytracing_pipeline_handle pipeline)
{
    auto table
        = sg::raytracing_shader_table_description{.pipeline = cc::move(pipeline), .ray_count = definition.ray_count};
    (void)table.add_raygen_shader(sg::raygen_shader_handle(0));
    for (auto r = 0; r < definition.ray_count; ++r)
        (void)table.add_miss_shader(sg::miss_shader_handle(r));
    return table;
}

sg::hit_row slib::add_hit_group_row(sg::raytracing_shader_table_description& table, int group)
{
    auto handles = cc::vector<sg::hit_shader_handle>();
    for (auto r = 0; r < table.ray_count; ++r)
        handles.push_back(sg::hit_shader_handle(group * table.ray_count + r));
    return table.add_hit_row(handles);
}
