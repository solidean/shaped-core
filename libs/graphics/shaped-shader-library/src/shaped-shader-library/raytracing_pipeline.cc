#include <clean-core/common/assertf.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

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

cc::shared_async<cc::vector<sg::hit_shader>> slib::compile_hit_group(sg::context* ctx,
                                                                     shader_library const* library,
                                                                     raytracing_pipeline_definition const* definition,
                                                                     cc::string source,
                                                                     cc::string group,
                                                                     cc::string label)
{
    auto const& d = *definition;
    auto const fail = [&](cc::string message)
    { return sg::pipeline_creation_exception(cc::string(group), cc::any_error(cc::move(message))); };

    auto const described = sgl::describe({.source = source, .source_name = label});
    if (described.has_error())
        throw fail(cc::format("{} does not compile, so it holds no hit group:\n{}", label, described.error()));
    auto const& m = described.value();
    auto const* found = static_cast<sgl::described_hit_group const*>(nullptr);
    for (auto const& g : m.hit_groups)
        if (g.name == group)
            found = &g;
    if (found == nullptr)
        throw fail(cc::format("{} declares no hit_group {}", label, group));
    // the payloads decide what a trace hands the group's shaders, so the ray set must match whole
    auto is_same_set = found->ray_set == d.ray_set;
    for (auto const& set : m.ray_sets)
        if (set.name == found->ray_set)
        {
            is_same_set = is_same_set && set.payloads.size() == d.payloads.size();
            for (auto i = isize(0); is_same_set && i < set.payloads.size(); ++i)
                is_same_set = set.payloads[i] == d.payloads[i];
        }
    if (!is_same_set)
        throw fail(cc::format("hit_group {} of {} is for the ray set {}, and {}'s {} traces {}", group, label,
                              found->ray_set, d.file, d.name, d.ray_set));

    // the context's first format this library builds SGL into, as an asset's acquire would pick
    auto format = cc::optional<sg::shader_format>();
    for (auto const f : ctx->accepted_shader_formats())
        if (!format.has_value() && library->can_compile(shader_language::sgl, f))
            format = f;
    if (!format.has_value())
        throw fail("no registered compiler builds SGL into a format this context accepts");

    auto const options = compile_source_options{.language = shader_language::sgl, .label = label};
    auto intersection = cc::optional<sg::compiled_shader>();
    if (!found->intersection.empty())
        intersection = co_await library->compile_source(source, sg::shader_stage::intersection, found->intersection,
                                                        format.value(), options);
    auto result = cc::vector<sg::hit_shader>();
    for (auto r = isize(0); r < d.ray_count; ++r)
    {
        auto shader = sg::hit_shader{.intersection = intersection};
        if (!found->closest_hits[r].empty())
            shader.closest_hit = co_await library->compile_source(source, sg::shader_stage::closest_hit,
                                                                  found->closest_hits[r], format.value(), options);
        if (!found->any_hits[r].empty())
            shader.any_hit = co_await library->compile_source(source, sg::shader_stage::any_hit, found->any_hits[r],
                                                              format.value(), options);
        result.push_back(cc::move(shader));
    }
    co_return result;
}
