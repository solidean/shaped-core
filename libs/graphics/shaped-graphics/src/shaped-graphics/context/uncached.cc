#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh> // cc::format
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/binding/binding.hh> // binding::count
#include <shaped-graphics/binding/impl/binding_conflicts.hh>
#include <shaped-graphics/binding/impl/portability.hh>
#include <shaped-graphics/binding/layout_fit.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/compute/compute_pipeline.hh> // compute_pipeline_description::shader
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/uncached.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>         // raster_pipeline_description
#include <shaped-graphics/raytracing/raytracing_pipeline.hh> // raytracing_pipeline_description

using namespace cc::primitive_defines;

namespace
{
// A shader is input, like a file: a hot reload hands in a new one while the program runs, and a generated layout from
// the build no longer fits an edited one.
// So what a shader gets wrong is a refusal the caller receives, and only what host code gets wrong asserts.

/// Why `shader` cannot be built against `layout`, or nothing when it fits.
cc::optional<cc::string> misfit_of(sg::compiled_shader const& shader, sg::pipeline_layout_handle const& layout)
{
    CC_ASSERT(layout != nullptr, "a pipeline needs a layout");
    auto misfit = sg::describe_layout_misfit(shader, *layout);
    if (misfit.empty())
        return {};
    return cc::format("the shader '{}' does not fit its pipeline layout:\n{}", shader.entry_point, misfit);
}

// What the frontend checks of a raster description before any backend sees it.
cc::optional<cc::string> refusal_of(sg::raster_pipeline_description const& desc)
{
    sg::compiled_shader const* const stages[] = {
        &desc.vertex_shader,
        desc.fragment_shader.has_value() ? &desc.fragment_shader.value() : nullptr,
        desc.tessellation_control_shader.has_value() ? &desc.tessellation_control_shader.value() : nullptr,
        desc.tessellation_evaluation_shader.has_value() ? &desc.tessellation_evaluation_shader.value() : nullptr,
        desc.geometry_shader.has_value() ? &desc.geometry_shader.value() : nullptr,
    };
    if (auto conflict = sg::impl::find_binding_conflict(stages); conflict.has_value())
        return conflict;
    for (auto const* stage : stages)
        if (stage != nullptr)
            if (auto misfit = misfit_of(*stage, desc.layout); misfit.has_value())
                return misfit;
    if (desc.fragment_shader.has_value())
    {
        auto const& fs = desc.fragment_shader.value();
        // A target no output writes is left undefined by every backend, unless its write mask keeps it unwritten.
        if (fs.color_output_count.has_value())
        {
            auto const written = isize(fs.color_output_count.value());
            if (written > desc.color_targets.size())
                return cc::format("the fragment shader '{}' writes {} color targets, and the pipeline has {}",
                                  fs.entry_point, written, desc.color_targets.size());
            for (auto i = written; i < desc.color_targets.size(); ++i)
                if (desc.color_targets[i].write_mask != sg::color_write_mask{})
                    return cc::format("color target {} of the pipeline for '{}' is written by no output of the shader, "
                                      "so its write mask must be empty",
                                      i, fs.entry_point);
        }
        if (!fs.target_set.empty() && !desc.target_set.empty() && fs.target_set != desc.target_set)
            return cc::format("the fragment shader '{}' writes '{}', and the pipeline names '{}'", fs.entry_point,
                              fs.target_set, desc.target_set);
    }

    // Such a pipeline still builds, and draws as if the state were off.
    auto const& ds = desc.depth_stencil;
    if ((ds.depth_test || ds.depth_write || ds.stencil_test) && desc.depth_stencil_format == sg::pixel_format::undefined)
        CC_LOG_WARNING("a raster pipeline for '{}' tests depth or stencil and has no depth_stencil_format, "
                       "so it draws without either",
                       desc.vertex_shader.entry_point);
    return {};
}

/// The target set a pipeline draws into: the description's, else what its fragment shader writes.
cc::string_view target_set_of(sg::raster_pipeline_description const& desc)
{
    if (!desc.target_set.empty() || !desc.fragment_shader.has_value())
        return desc.target_set;
    return desc.fragment_shader.value().target_set;
}

cc::shared_async<sg::raster_pipeline_handle> named(cc::shared_async<sg::raster_pipeline_handle> built,
                                                   cc::string target_set)
{
    auto pipeline = co_await built;
    sg::impl::set_target_set(*pipeline, target_set);
    co_return pipeline;
}
} // namespace

namespace sg
{
binding_group_layout_handle context_uncached_scope::create_binding_group_layout(cc::span<binding const> bindings,
                                                                                cc::span<named_sampler const> static_samplers)
{
    auto r = try_create_binding_group_layout(bindings, static_samplers);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception("", r.error());
}

cc::result<binding_group_layout_handle> context_uncached_scope::try_create_binding_group_layout(
    cc::span<binding const> bindings,
    cc::span<named_sampler const> static_samplers)
{
    // Rejected here rather than per backend, so an unbounded array fails identically everywhere: WebGPU has no
    // such thing, and sg holds the portable floor until it does.
    // See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
    // An error rather than an assert — these bindings usually come from reflecting someone's shader, which
    // makes an unbounded array content, not a contract violation.
    for (auto const& b : bindings)
        if (b.count == 0)
            return cc::error(cc::format("binding_group_layout: '{}' is an unbounded array (count 0), which sg does "
                                        "not support — declare a bounded count and treat it as capacity",
                                        b.name));

    if (auto unsupported = impl::find_unsupported_binding(_ctx, bindings); unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));

    return _ctx.try_create_binding_group_layout(bindings, static_samplers, lifetime_scope::persistent);
}

pipeline_layout_handle context_uncached_scope::create_pipeline_layout(pipeline_layout_description const& desc)
{
    auto r = try_create_pipeline_layout(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception("", r.error());
}

cc::result<pipeline_layout_handle> context_uncached_scope::try_create_pipeline_layout(pipeline_layout_description const& desc)
{
    return _ctx.try_create_pipeline_layout(desc, lifetime_scope::persistent);
}

compute_pipeline_handle context_uncached_scope::create_compute_pipeline(compute_pipeline_description const& desc)
{
    auto r = try_create_compute_pipeline(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception(desc.shader.entry_point, r.error());
}

cc::result<compute_pipeline_handle> context_uncached_scope::try_create_compute_pipeline(
    compute_pipeline_description const& desc)
{
    if (auto misfit = misfit_of(desc.shader, desc.layout); misfit.has_value())
        return cc::error(cc::move(misfit.value()));
    return _ctx.try_create_compute_pipeline(desc, lifetime_scope::persistent);
}

raster_pipeline_handle context_uncached_scope::create_raster_pipeline(raster_pipeline_description const& desc)
{
    auto r = try_create_raster_pipeline(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception(desc.vertex_shader.entry_point, r.error());
}

cc::result<raster_pipeline_handle> context_uncached_scope::try_create_raster_pipeline(raster_pipeline_description const& desc)
{
    if (auto refusal = refusal_of(desc); refusal.has_value())
        return cc::error(cc::move(refusal.value()));

    auto r = _ctx.try_create_raster_pipeline(desc, lifetime_scope::persistent);
    if (r.has_value())
        impl::set_target_set(*r.value(), target_set_of(desc));
    return r;
}

cc::shared_async<compute_pipeline_handle> context_uncached_scope::create_compute_pipeline_async(
    compute_pipeline_description const& desc)
{
    if (auto misfit = misfit_of(desc.shader, desc.layout); misfit.has_value())
        return cc::make_async_from_error<compute_pipeline_handle>(
            cc::async_error::make_error(cc::any_error(cc::move(misfit.value()))));
    return _ctx.create_compute_pipeline_async(desc, lifetime_scope::persistent);
}

cc::shared_async<raster_pipeline_handle> context_uncached_scope::create_raster_pipeline_async(
    raster_pipeline_description const& desc)
{
    if (auto refusal = refusal_of(desc); refusal.has_value())
        return cc::make_async_from_error<raster_pipeline_handle>(
            cc::async_error::make_error(cc::any_error(cc::move(refusal.value()))));

    // A backend may settle the build from a callback of its own, so the name is set once it has.
    auto built = _ctx.create_raster_pipeline_async(desc, lifetime_scope::persistent);
    auto const target_set = target_set_of(desc);
    if (target_set.empty())
        return built;
    return named(cc::move(built), cc::string(target_set));
}

raytracing_pipeline_handle context_uncached_scope::create_raytracing_pipeline(raytracing_pipeline_description const& desc)
{
    auto r = try_create_raytracing_pipeline(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    cc::string const label = desc.raygen_shaders.empty() ? cc::string("") : desc.raygen_shaders.front().entry_point;
    throw pipeline_creation_exception(label, r.error());
}

cc::result<raytracing_pipeline_handle> context_uncached_scope::try_create_raytracing_pipeline(
    raytracing_pipeline_description const& desc)
{
    // Ray tracing is where this earns its keep: a pipeline's shaders naturally live in separate files, so nothing
    // but a shared header makes them agree about a group's numbering.
    cc::vector<compiled_shader const*> stages;
    for (auto const& s : desc.raygen_shaders)
        stages.push_back(&s);
    for (auto const& s : desc.miss_shaders)
        stages.push_back(&s);
    for (auto const& s : desc.callable_shaders)
        stages.push_back(&s);
    for (auto const& g : desc.hit_shaders)
    {
        if (g.closest_hit.has_value())
            stages.push_back(&g.closest_hit.value());
        if (g.any_hit.has_value())
            stages.push_back(&g.any_hit.value());
        if (g.intersection.has_value())
            stages.push_back(&g.intersection.value());
    }
    if (auto conflict = impl::find_binding_conflict(stages); conflict.has_value())
        return cc::error(cc::move(conflict.value()));

    return _ctx.try_create_raytracing_pipeline(desc, lifetime_scope::persistent);
}

raytracing_shader_table_handle context_uncached_scope::create_raytracing_shader_table(
    raytracing_shader_table_description const& desc)
{
    auto r = try_create_raytracing_shader_table(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception("raytracing_shader_table", r.error());
}

cc::result<raytracing_shader_table_handle> context_uncached_scope::try_create_raytracing_shader_table(
    raytracing_shader_table_description const& desc)
{
    return _ctx.try_create_raytracing_shader_table(desc, lifetime_scope::persistent);
}
} // namespace sg
