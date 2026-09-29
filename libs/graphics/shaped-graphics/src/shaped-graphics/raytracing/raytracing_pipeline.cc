#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>

namespace sg
{
raytracing_pipeline::~raytracing_pipeline() = default;

void impl::set_footprint(raytracing_pipeline const& pipeline, impl::pipeline_footprint footprint)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    const_cast<raytracing_pipeline&>(pipeline)._footprint = cc::move(footprint);
}

void impl::set_hit_groups(raytracing_pipeline const& pipeline, cc::span<hit_shader const> groups)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    auto& procedural = const_cast<raytracing_pipeline&>(pipeline)._procedural_hit_groups;
    procedural.clear();
    for (auto const& g : groups)
        procedural.push_back(g.intersection.has_value());
}

raygen_shader_handle raytracing_pipeline_description::add_raygen_shader(compiled_shader shader)
{
    CC_ASSERT(shader.stage == shader_stage::raygen, "add_raygen_shader requires a raygen shader");
    auto const handle = raygen_shader_handle(u32(raygen_shaders.size()));
    raygen_shaders.push_back(cc::move(shader));
    return handle;
}

miss_shader_handle raytracing_pipeline_description::add_miss_shader(compiled_shader shader)
{
    CC_ASSERT(shader.stage == shader_stage::miss, "add_miss_shader requires a miss shader");
    auto const handle = miss_shader_handle(u32(miss_shaders.size()));
    miss_shaders.push_back(cc::move(shader));
    return handle;
}

callable_shader_handle raytracing_pipeline_description::add_callable_shader(compiled_shader shader)
{
    CC_ASSERT(shader.stage == shader_stage::callable, "add_callable_shader requires a callable shader");
    auto const handle = callable_shader_handle(u32(callable_shaders.size()));
    callable_shaders.push_back(cc::move(shader));
    return handle;
}

hit_shader_handle raytracing_pipeline_description::add_hit_shader(hit_shader shader)
{
    CC_ASSERT(!shader.closest_hit.has_value() || shader.closest_hit.value().stage == shader_stage::closest_hit,
              "hit_shader.closest_hit must be a closest_hit shader");
    CC_ASSERT(!shader.any_hit.has_value() || shader.any_hit.value().stage == shader_stage::any_hit,
              "hit_shader.any_hit must be an any_hit shader");
    CC_ASSERT(!shader.intersection.has_value() || shader.intersection.value().stage == shader_stage::intersection,
              "hit_shader.intersection must be an intersection shader");
    auto const handle = hit_shader_handle(u32(hit_shaders.size()));
    hit_shaders.push_back(cc::move(shader));
    return handle;
}
} // namespace sg
