#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_array.hh>
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
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

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

/// `desc` over the twin of its layout that holds the grid, where its shader is dispatched by threads alone, which
/// reads it; `desc` itself otherwise.
sg::compute_pipeline_description over_grid(sg::context& ctx, sg::compute_pipeline_description const& desc)
{
    if (!desc.shader.is_dispatched_by_threads || desc.layout == nullptr || desc.layout->has_dispatch_grid())
        return desc;
    auto twin = desc.layout->description();
    twin.dispatch_grid = true;
    return {.shader = desc.shader,
            .layout = ctx.cached.acquire_pipeline_layout(twin),
            .cached_pipeline = desc.cached_pipeline};
}

/// A polyfilled description's raster half, over the twin of its layout that holds `sg_mesh_input`; `desc` otherwise.
sg::raster_pipeline_description raster_half_of(sg::context& ctx, sg::raster_pipeline_description const& desc)
{
    if (!desc.mesh_polyfill.has_value() || desc.layout == nullptr
        || desc.layout->mesh_polyfill() == sg::mesh_polyfill_slots::input)
        return desc;
    auto twin = desc.layout->description();
    twin.mesh_polyfill = sg::mesh_polyfill_slots::input;
    auto result = desc;
    result.layout = ctx.cached.acquire_pipeline_layout(twin);
    return result;
}

/// A polyfilled description's compute half `shader`, over the twin of its layout that holds `sg_mesh_output` and the
/// grid; a half launched by records reads them instead of the grid, since it is dispatched indirectly.
sg::compute_pipeline_description compute_half_of(sg::context& ctx,
                                                 sg::raster_pipeline_description const& desc,
                                                 sg::compiled_shader const& shader,
                                                 bool is_launched)
{
    auto twin = desc.layout->description();
    twin.mesh_polyfill = is_launched ? sg::mesh_polyfill_slots::launched : sg::mesh_polyfill_slots::output;
    twin.dispatch_grid = !is_launched;
    return {.shader = shader, .layout = ctx.cached.acquire_pipeline_layout(twin)};
}

sg::compute_pipeline_description compute_half_of(sg::context& ctx, sg::raster_pipeline_description const& desc)
{
    auto const& p = desc.mesh_polyfill.value();
    return compute_half_of(ctx, desc, p.compute_shader, p.task_shader.has_value());
}

/// A polyfilled task stage's compute form or its scan, over the twin of its layout that holds `sg_task_output` and the
/// grid.
sg::compute_pipeline_description task_half_of(sg::context& ctx,
                                              sg::raster_pipeline_description const& desc,
                                              sg::compiled_shader const& shader)
{
    auto twin = desc.layout->description();
    twin.mesh_polyfill = sg::mesh_polyfill_slots::task;
    twin.dispatch_grid = true;
    return {.shader = shader, .layout = ctx.cached.acquire_pipeline_layout(twin)};
}

/// The pipelines a polyfill dispatches before its draw: the mesh stage's compute half, a task stage's compute form,
/// the half an indirect draw with no task stage launches, and the scan of their records.
struct polyfill_halves
{
    sg::compute_pipeline_handle compute;
    sg::compute_pipeline_handle task;
    sg::compute_pipeline_handle launched;
    sg::compute_pipeline_handle scan;
};

sg::impl::mesh_polyfill_state state_of(sg::mesh_polyfill_description const& polyfill, polyfill_halves halves)
{
    return {.compute = cc::move(halves.compute),
            .workgroup = polyfill.compute_shader.workgroup_size.value_or(sg::compute_dimensions{}),
            .corners = polyfill.corners,
            .primitives_per_group = polyfill.primitives_per_group,
            .words_per_group = polyfill.words_per_group,
            .input_corners = polyfill.input_corners,
            .task = cc::move(halves.task),
            .launched = cc::move(halves.launched),
            .scan = cc::move(halves.scan),
            .task_workgroup = polyfill.task_shader.has_value()
                                ? polyfill.task_shader.value().workgroup_size.value_or(sg::compute_dimensions{})
                                : sg::compute_dimensions{},
            .record_words = polyfill.record_words,
            .task_meshes = polyfill.task_meshes,
            .launch_budget = polyfill.launch_budget};
}

/// Why a polyfilled mesh or geometry stage of `desc` cannot be built, or nothing when it can; nothing for a description
/// without one.
cc::optional<cc::string> polyfill_refusal_of(sg::raster_pipeline_description const& desc, sg::context const& ctx)
{
    if (!desc.mesh_polyfill.has_value())
        return {};
    auto const& p = desc.mesh_polyfill.value();
    if (p.input_corners > 0)
    {
        if (!ctx.draws_geometry_polyfill())
            return cc::string("the pipeline polyfills its geometry stage, and this backend draws no polyfilled "
                              "geometry "
                              "stage yet");
        if (p.input_corners > 3)
            return cc::format("the pipeline's geometry polyfill assembles primitives of {} corners, and a point has 1, "
                              "a line 2, a triangle 3",
                              p.input_corners);
    }
    else if (!ctx.draws_mesh_polyfill())
        return cc::string("the pipeline polyfills its mesh stage, and this backend draws no polyfilled mesh stage yet");
    if (desc.mesh_shader.has_value() || desc.task_shader.has_value())
        return cc::string("the pipeline polyfills its mesh stage and holds a mesh or task stage too; the polyfill "
                          "takes "
                          "the mesh stage's place");
    if (desc.tessellation_control_shader.has_value() || desc.tessellation_evaluation_shader.has_value()
        || desc.geometry_shader.has_value())
        return cc::string("the pipeline polyfills its mesh stage beside a tessellation or geometry stage");
    if (!desc.vertex_input.slots.empty())
        return cc::string("the pipeline polyfills its mesh stage and has a vertex input; its vertex half reads the "
                          "polyfill's slots");
    if (desc.view_count > 1)
        return cc::string("the pipeline renders several views from a polyfilled mesh stage, which sg does not wire up "
                          "yet");
    if (p.corners != 2 && p.corners != 3)
        return cc::format("the pipeline's mesh polyfill has {} corners a primitive, and a line has 2, a triangle 3",
                          p.corners);
    auto const topology = p.corners == 3 ? sg::primitive_topology::triangle_list : sg::primitive_topology::line_list;
    if (desc.topology != topology)
        return cc::string("the pipeline's topology is not the list its mesh polyfill's primitives draw as");
    if (p.primitives_per_group <= 0 || p.words_per_group <= 0)
        return cc::string("the pipeline's mesh polyfill states no primitives or no words per workgroup");
    if (p.compute_shader.bytecode.empty())
        return cc::string("the pipeline's mesh polyfill has no compute half");
    if (p.task_shader.has_value() || p.launched_shader.has_value())
    {
        if (p.input_corners > 0)
            return cc::string("the pipeline polyfills a geometry stage, which no task stage or indirect draw launches");
        if (p.task_shader.has_value() && p.launched_shader.has_value())
            return cc::string("the pipeline's mesh polyfill has a task stage and a launched half, and behind a task "
                              "stage its compute half is the launched one");
        if (p.task_shader.has_value() && p.task_shader.value().bytecode.empty())
            return cc::string("the pipeline's task polyfill has no compute form");
        if (p.launched_shader.has_value() && p.launched_shader.value().bytecode.empty())
            return cc::string("the pipeline's mesh polyfill has an empty launched half");
        if (p.scan_shader.bytecode.empty())
            return cc::string("the pipeline's mesh polyfill launches by records and has no scan of them");
        if (p.record_words < 4)
            return cc::string("the pipeline's mesh polyfill states no words per record");
        if (p.launch_budget <= 0 || p.launch_budget > sg::max_dispatch_groups_per_dimension)
            return cc::format("the pipeline's mesh polyfill has a budget of {} workgroups a draw, which is not from 1 "
                              "to {}",
                              p.launch_budget, sg::max_dispatch_groups_per_dimension);
    }
    return {};
}

cc::shared_async<sg::raster_pipeline_handle> with_polyfill(cc::shared_async<sg::raster_pipeline_handle> built,
                                                           cc::shared_async<sg::compute_pipeline_handle> compute,
                                                           cc::shared_async<sg::compute_pipeline_handle> task,
                                                           cc::shared_async<sg::compute_pipeline_handle> launched,
                                                           cc::shared_async<sg::compute_pipeline_handle> scan,
                                                           sg::mesh_polyfill_description numbers)
{
    auto pipeline = co_await built;
    auto halves = polyfill_halves{.compute = co_await compute};
    if (task != nullptr)
        halves.task = co_await task;
    if (launched != nullptr)
        halves.launched = co_await launched;
    if (scan != nullptr)
        halves.scan = co_await scan;
    numbers.compute_shader = {};
    numbers.scan_shader = {};
    if (numbers.task_shader.has_value())
        numbers.task_shader.value().bytecode = {};
    if (numbers.launched_shader.has_value())
        numbers.launched_shader.value().bytecode = {};
    sg::impl::set_mesh_polyfill(*pipeline, state_of(numbers, cc::move(halves)));
    co_return pipeline;
}

/// The compute passes of a polyfilled tessellation, each over the pipeline's layout; the control pass, dispatched by
/// threads, over its twin that holds the grid.
struct tessellation_passes
{
    sg::compute_pipeline_description control;
    sg::compute_pipeline_description count;
    sg::compute_pipeline_description scan;
    sg::compute_pipeline_description generate;
    sg::compute_pipeline_description evaluation;
};

tessellation_passes tessellation_passes_of(sg::context& ctx, sg::raster_pipeline_description const& desc)
{
    auto const& p = desc.tessellation_polyfill.value();
    auto const over
        = [&](sg::compiled_shader const& shader) { return over_grid(ctx, {.shader = shader, .layout = desc.layout}); };
    return {.control = over(p.control_shader),
            .count = over(p.count_shader),
            .scan = over(p.scan_shader),
            .generate = over(p.generate_shader),
            .evaluation = over(p.evaluation_shader)};
}

sg::impl::tessellation_polyfill_state tessellation_state_of(sg::tessellation_polyfill_description const& p)
{
    return {.domain = p.domain,
            .control_points = p.control_points,
            .patch_words = p.patch_words,
            .vertex_words = p.vertex_words,
            .partitioning = p.partitioning,
            .output = p.output,
            .max_factor = p.max_factor};
}

/// Why a geometry stage's mesh route of `desc` cannot be built, or nothing when it can; nothing for a description
/// without one (SGL's CHK-525).
cc::optional<cc::string> geometry_mesh_refusal_of(sg::raster_pipeline_description const& desc, sg::context const& ctx)
{
    if (desc.geometry_mesh_corners == 0)
        return {};
    if (!ctx.draws_geometry_mesh_route())
        return cc::string("the pipeline draws its geometry stage as a mesh stage, and this backend draws no such "
                          "pipeline yet");
    if (desc.geometry_mesh_corners < 1 || desc.geometry_mesh_corners > 3
        || (desc.topology != sg::primitive_topology::triangle_list && desc.topology != sg::primitive_topology::line_list))
        return cc::string("the pipeline's geometry mesh route assembles points, lines or triangles, and draws a "
                          "triangle or a line list");
    if (!desc.mesh_shader.has_value() || desc.task_shader.has_value() || desc.mesh_polyfill.has_value()
        || desc.tessellation_polyfill.has_value())
        return cc::string("the pipeline's geometry mesh route is its mesh stage alone, with no task stage or other "
                          "polyfill");
    if (desc.view_count > 1)
        return cc::string("the pipeline renders several views from a geometry mesh route, which sg does not wire up "
                          "yet");
    return {};
}

/// Why a polyfilled tessellation of `desc` cannot be built, or nothing when it can; nothing for a description without
/// one (SGL's CHK-527).
cc::optional<cc::string> tessellation_refusal_of(sg::raster_pipeline_description const& desc, sg::context const& ctx)
{
    if (!desc.tessellation_polyfill.has_value())
        return {};
    auto const& p = desc.tessellation_polyfill.value();
    if (!ctx.draws_tessellation_polyfill())
        return cc::string("the pipeline polyfills its tessellation stages, and this backend draws no polyfilled "
                          "tessellation yet");
    if (desc.tessellation_control_shader.has_value() || desc.tessellation_evaluation_shader.has_value()
        || desc.geometry_shader.has_value() || desc.mesh_shader.has_value() || desc.mesh_polyfill.has_value())
        return cc::string("the pipeline polyfills its tessellation stages beside native tessellation, geometry or mesh "
                          "stages");
    if (!desc.vertex_input.slots.empty())
        return cc::string("the pipeline polyfills its tessellation stages and has a vertex input; its control pass "
                          "reads "
                          "the bound buffers itself");
    if (desc.view_count > 1)
        return cc::string("the pipeline renders several views from polyfilled tessellation, which sg does not wire up "
                          "yet");
    if (p.domain < 2 || p.domain > 4 || p.control_points < 1 || p.control_points > 32 || p.patch_words <= 0
        || p.vertex_words <= 0)
        return cc::string("the pipeline's tessellation polyfill states no domain, control points or words");
    if (!(p.max_factor >= 1.0f && p.max_factor <= 64.0f))
        return cc::format("the pipeline's tessellation polyfill holds room for factors up to {}, and D3D's are from 1 "
                          "to 64",
                          p.max_factor);
    auto const topology = p.output == 2 ? sg::primitive_topology::line_list : sg::primitive_topology::triangle_list;
    if (desc.topology != topology)
        return cc::string("the pipeline's topology is not the list its tessellation polyfill's primitives draw as");
    for (auto const* const shader :
         {&p.control_shader, &p.count_shader, &p.scan_shader, &p.generate_shader, &p.evaluation_shader})
        if (shader->bytecode.empty())
            return cc::string("the pipeline's tessellation polyfill lacks one of its compute passes");
    return {};
}

cc::shared_async<sg::raster_pipeline_handle> with_tessellation(
    cc::shared_async<sg::raster_pipeline_handle> built,
    cc::fixed_array<cc::shared_async<sg::compute_pipeline_handle>, 5> passes,
    sg::impl::tessellation_polyfill_state state)
{
    auto pipeline = co_await built;
    state.control = co_await passes[0];
    state.count = co_await passes[1];
    state.scan = co_await passes[2];
    state.generate = co_await passes[3];
    state.evaluation = co_await passes[4];
    sg::impl::set_tessellation_polyfill(*pipeline, cc::move(state));
    co_return pipeline;
}

/// Why a compute pipeline of `desc` cannot be built on a device with `supported`, or nothing when it can.
cc::optional<cc::string> refusal_of(sg::compute_pipeline_description const& desc, sg::feature_set supported)
{
    sg::compiled_shader const* const stages[] = {&desc.shader};
    if (auto missing = sg::impl::find_missing_feature(supported, stages); missing.has_value())
        return missing;
    if (auto reserved = sg::impl::find_reserved_slot(stages); reserved.has_value())
        return reserved;
    return misfit_of(desc.shader, desc.layout);
}

/// The name a raster pipeline's errors give it: its vertex stage's, or its mesh stage's.
cc::string_view entry_point_of(sg::raster_pipeline_description const& desc)
{
    return desc.mesh_shader.has_value() ? cc::string_view(desc.mesh_shader.value().entry_point)
                                        : cc::string_view(desc.vertex_shader.entry_point);
}

struct raster_stages
{
    sg::compiled_shader const* at[7];
};

/// Every stage `desc` sets, null where one is not: the vertex stage, or the mesh stage in its place.
raster_stages stages_of(sg::raster_pipeline_description const& desc)
{
    auto const optional = [](cc::optional<sg::compiled_shader> const& s) { return s.has_value() ? &s.value() : nullptr; };
    return {{desc.mesh_shader.has_value() ? nullptr : &desc.vertex_shader, optional(desc.fragment_shader),
             optional(desc.tessellation_control_shader), optional(desc.tessellation_evaluation_shader),
             optional(desc.geometry_shader), optional(desc.task_shader), optional(desc.mesh_shader)}};
}

// What the frontend checks of a raster description before any backend sees it.
cc::optional<cc::string> refusal_of(sg::raster_pipeline_description const& desc, sg::feature_set supported)
{
    auto const listed = stages_of(desc);
    auto const stages = cc::span<sg::compiled_shader const* const>(listed.at);
    // a stage the device lacks is refused by its feature, whatever language its shader was written in
    auto const has_tessellation
        = desc.tessellation_control_shader.has_value() || desc.tessellation_evaluation_shader.has_value();
    if (has_tessellation && !supported.has(sg::feature::tessellation_shader))
        return cc::string("the pipeline has tessellation stages, and this device lacks "
                          "sg::feature::tessellation_shader");
    if (desc.geometry_shader.has_value() && !supported.has(sg::feature::geometry_shader))
        return cc::string("the pipeline has a geometry stage, and this device lacks sg::feature::geometry_shader");
    if (desc.task_shader.has_value() && !desc.mesh_shader.has_value())
        return cc::string("the pipeline has a task stage and no mesh stage for it to launch");
    if (desc.mesh_shader.has_value())
    {
        if (!supported.has(sg::feature::mesh_shader))
            return cc::string("the pipeline has a mesh stage, and this device lacks sg::feature::mesh_shader");
        if (!desc.vertex_shader.bytecode.empty())
            return cc::string("the pipeline has a mesh stage and a vertex stage; the mesh stage takes the vertex "
                              "stage's place");
        if (has_tessellation || desc.geometry_shader.has_value())
            return cc::string("the pipeline has a mesh stage beside a tessellation or geometry stage");
        if (!desc.vertex_input.slots.empty())
            return cc::string("the pipeline has a mesh stage and a vertex input; a mesh stage reads its "
                              "vertices from bindings");
        if (desc.view_count > 1)
            return cc::string("the pipeline renders several views from a mesh stage, which sg does not wire up yet");
    }
    if (desc.view_count > 1 && !supported.has(sg::feature::view_instancing))
        return cc::string("the pipeline renders several views, and this device lacks sg::feature::view_instancing");
    if (desc.rasterization.fill == sg::fill_mode::wireframe && !supported.has(sg::feature::wireframe_fill))
        return cc::string("the pipeline fills wireframe, and this device lacks sg::feature::wireframe_fill");
    if (desc.depth_stencil_format == sg::pixel_format::depth32_float_stencil8
        && !supported.has(sg::feature::depth32_float_stencil8))
        return cc::string("the pipeline's depth-stencil format is depth32_float_stencil8, and this device lacks "
                          "sg::feature::depth32_float_stencil8");
    if (auto conflict = sg::impl::find_binding_conflict(stages); conflict.has_value())
        return conflict;
    if (auto missing = sg::impl::find_missing_feature(supported, stages); missing.has_value())
        return missing;
    if (auto reserved = sg::impl::find_reserved_slot(stages); reserved.has_value())
        return reserved;
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
                       entry_point_of(desc));
    return {};
}

/// The target set a pipeline draws into: the description's, else what its fragment shader writes.
cc::string_view target_set_of(sg::raster_pipeline_description const& desc)
{
    if (!desc.target_set.empty() || !desc.fragment_shader.has_value())
        return desc.target_set;
    return desc.fragment_shader.value().target_set;
}

/// The targets a pipeline built from `desc` must be bound with.
sg::raster_target_formats target_formats_of(sg::raster_pipeline_description const& desc)
{
    auto formats = sg::raster_target_formats{.depth_stencil = desc.depth_stencil_format,
                                             .sample_count = desc.sample_count,
                                             .view_count = desc.view_count};
    for (auto const& target : desc.color_targets)
        formats.color.push_back(target.format);
    return formats;
}

/// What a pipeline's code does to each binding: every stage's footprint, resolved against `layout`.
sg::impl::pipeline_footprint footprint_of(sg::pipeline_layout const& layout,
                                          cc::span<sg::compiled_shader const* const> shaders)
{
    cc::vector<sg::impl::pipeline_footprint::stage_input> stages;
    for (auto const* shader : shaders)
        if (shader != nullptr)
            stages.push_back({.footprint = &shader->footprint,
                              .stages = sg::impl::stages_of(shader->stage),
                              .writes_debug = !shader->debug_sites.empty()});
    return sg::impl::pipeline_footprint::resolve(layout, stages);
}

/// Makes every print and assert of `shaders` known to the context that decodes their records.
void add_debug_sites(sg::impl::debug_windows& windows, cc::span<sg::compiled_shader const* const> shaders)
{
    for (auto const* shader : shaders)
        if (shader != nullptr)
            windows.add_sites(shader->debug_sites);
}

void add_debug_sites(sg::impl::debug_windows& windows, sg::compute_pipeline_description const& desc)
{
    sg::compiled_shader const* const stages[] = {&desc.shader};
    add_debug_sites(windows, stages);
}

void add_debug_sites(sg::impl::debug_windows& windows, sg::raster_pipeline_description const& desc)
{
    auto const listed = stages_of(desc);
    auto const stages = cc::span<sg::compiled_shader const* const>(listed.at);
    add_debug_sites(windows, stages);
}

sg::impl::pipeline_footprint footprint_of(sg::compute_pipeline_description const& desc)
{
    sg::compiled_shader const* const stages[] = {&desc.shader};
    return footprint_of(*desc.layout, stages);
}

sg::impl::pipeline_footprint footprint_of(sg::raster_pipeline_description const& desc)
{
    auto const listed = stages_of(desc);
    auto const stages = cc::span<sg::compiled_shader const* const>(listed.at);
    return footprint_of(*desc.layout, stages);
}

/// Why every backend refuses `desc`'s bound samplers, or none where it takes them.
/// A register is one slot whatever its space, since only dx12 has spaces to tell two apart.
cc::optional<cc::string> bound_sampler_refusal(sg::pipeline_layout_description const& desc)
{
    for (auto i = isize(0); i < desc.static_samplers.size(); ++i)
    {
        auto const& b = desc.static_samplers[i].binding;
        if (!sg::is_sampler(b.type) || b.count != 1)
            return cc::format("pipeline_layout: bound sampler '{}' must be a binding of one sampler", b.name);
        if (int(b.index) >= sg::max_bound_samplers)
            return cc::format("pipeline_layout: bound sampler '{}' takes register {}, and a pipeline holds {} on every "
                              "backend",
                              b.name, b.index, sg::max_bound_samplers);
        for (auto j = isize(0); j < i; ++j)
            if (desc.static_samplers[j].binding.index == b.index)
                return cc::format("pipeline_layout: bound samplers '{}' and '{}' both take register {}",
                                  desc.static_samplers[j].binding.name, b.name, b.index);
    }
    return {};
}

cc::shared_async<sg::raster_pipeline_handle> named(cc::shared_async<sg::raster_pipeline_handle> built,
                                                   cc::string target_set,
                                                   sg::raster_target_formats formats,
                                                   bool is_mesh_shaded,
                                                   sg::impl::pipeline_footprint footprint,
                                                   int geometry_mesh_corners)
{
    auto pipeline = co_await built;
    sg::impl::set_targets(*pipeline, target_set, formats, is_mesh_shaded);
    if (geometry_mesh_corners > 0)
        sg::impl::set_geometry_mesh_corners(*pipeline, geometry_mesh_corners);
    sg::impl::set_footprint(*pipeline, cc::move(footprint));
    co_return pipeline;
}

cc::shared_async<sg::compute_pipeline_handle> with_footprint(cc::shared_async<sg::compute_pipeline_handle> built,
                                                             sg::impl::pipeline_footprint footprint,
                                                             bool is_dispatched_by_threads)
{
    auto pipeline = co_await built;
    sg::impl::set_footprint(*pipeline, cc::move(footprint));
    if (is_dispatched_by_threads)
        sg::impl::set_dispatched_by_threads(*pipeline);
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
    // Judged here rather than per backend, so an unbounded array fails identically everywhere it fails.
    // Vulkan sizes only a set's last binding per set, so it stands last, and at the highest index; see
    // libs/graphics/shaped-graphics/docs/concepts/bindings.md.
    // An error rather than an assert: these bindings usually come from reflecting someone's shader.
    for (auto i = isize(0); i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        if (!b.is_unbounded())
            continue;
        if (!_ctx.supports(feature::unbounded_binding_arrays))
            return cc::error(cc::format("binding_group_layout: '{}' is an unbounded array (count 0), which needs "
                                        "sg::feature::unbounded_binding_arrays",
                                        b.name));
        if (i != bindings.size() - 1)
            return cc::error(cc::format("binding_group_layout: '{}' is an unbounded array, and only the last binding "
                                        "of a layout may be one",
                                        b.name));
        for (auto const& other : bindings)
            if (&other != &b && other.index > b.index)
                return cc::error(cc::format("binding_group_layout: '{}' is an unbounded array below '{}', and it must "
                                            "take the layout's highest index",
                                            b.name, other.name));
        if (sg::is_sampler(b.type) || b.type == binding_type::constants_buffer
            || b.type == binding_type::acceleration_structure)
            return cc::error(cc::format("binding_group_layout: '{}' is an unbounded array of a kind no binding array "
                                        "holds",
                                        b.name));
    }

    for (auto const& b : bindings)
    {
        if (!sg::is_sampler(b.type) || b.count == 1)
            continue;
        if (!_ctx.supports(feature::binding_arrays))
            return cc::error(cc::format("binding_group_layout: '{}' is an array of samplers, which needs "
                                        "sg::feature::binding_arrays",
                                        b.name));
        for (auto const& s : static_samplers)
            if (s.name == b.name)
                return cc::error(cc::format("binding_group_layout: '{}' is an array of samplers, which is supplied per "
                                            "group and is never static",
                                            b.name));
    }

    if (auto unsupported = impl::find_unsupported_binding(_ctx.supports(feature::extended_image_formats), bindings);
        unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));

    auto created = _ctx.try_create_binding_group_layout(bindings, static_samplers, lifetime_scope::persistent);
    if (created.has_value())
        _ctx.note_array_layout(created.value());
    return created;
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
    if (auto refusal = bound_sampler_refusal(desc); refusal.has_value())
        return cc::error(cc::move(refusal.value()));
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
    compute_pipeline_description const& given)
{
    auto const desc = over_grid(_ctx, given);
    if (auto refusal = refusal_of(desc, _ctx.supported_features()); refusal.has_value())
        return cc::error(cc::move(refusal.value()));
    _ctx._stats.add(stat::pipelines_created);
    auto r = _ctx.try_create_compute_pipeline(desc, lifetime_scope::persistent);
    if (r.has_value())
    {
        add_debug_sites(_ctx._debug_windows, desc);
        impl::set_footprint(*r.value(), footprint_of(desc));
        if (desc.shader.is_dispatched_by_threads)
            impl::set_dispatched_by_threads(*r.value());
    }
    return r;
}

raster_pipeline_handle context_uncached_scope::create_raster_pipeline(raster_pipeline_description const& desc)
{
    auto r = try_create_raster_pipeline(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception(entry_point_of(desc), r.error());
}

cc::result<raster_pipeline_handle> context_uncached_scope::try_create_raster_pipeline(raster_pipeline_description const& given)
{
    auto const desc = raster_half_of(_ctx, given);
    if (auto refusal = refusal_of(desc, _ctx.supported_features()); refusal.has_value())
        return cc::error(cc::move(refusal.value()));
    if (auto refusal = polyfill_refusal_of(desc, _ctx); refusal.has_value())
        return cc::error(cc::move(refusal.value()));
    if (auto refusal = tessellation_refusal_of(desc, _ctx); refusal.has_value())
        return cc::error(cc::move(refusal.value()));
    if (auto refusal = geometry_mesh_refusal_of(desc, _ctx); refusal.has_value())
        return cc::error(cc::move(refusal.value()));

    // the compute halves first, so a refusal of one leaves no raster half behind
    auto halves = polyfill_halves();
    if (desc.mesh_polyfill.has_value())
    {
        auto half = try_create_compute_pipeline(compute_half_of(_ctx, desc));
        if (half.has_error())
            return cc::error(cc::format("its mesh polyfill's compute half: {}", half.error()));
        halves.compute = cc::move(half.value());
        auto const& p = desc.mesh_polyfill.value();
        if (p.task_shader.has_value())
        {
            auto task = try_create_compute_pipeline(task_half_of(_ctx, desc, p.task_shader.value()));
            if (task.has_error())
                return cc::error(cc::format("its task polyfill's compute form: {}", task.error()));
            halves.task = cc::move(task.value());
        }
        if (p.launched_shader.has_value())
        {
            auto launched = try_create_compute_pipeline(compute_half_of(_ctx, desc, p.launched_shader.value(), true));
            if (launched.has_error())
                return cc::error(cc::format("its mesh polyfill's launched half: {}", launched.error()));
            halves.launched = cc::move(launched.value());
        }
        if (p.task_shader.has_value() || p.launched_shader.has_value())
        {
            auto scan = try_create_compute_pipeline(task_half_of(_ctx, desc, p.scan_shader));
            if (scan.has_error())
                return cc::error(cc::format("its mesh polyfill's scan: {}", scan.error()));
            halves.scan = cc::move(scan.value());
        }
    }

    auto tessellation = cc::optional<impl::tessellation_polyfill_state>();
    if (desc.tessellation_polyfill.has_value())
    {
        auto const passes = tessellation_passes_of(_ctx, desc);
        auto state = tessellation_state_of(desc.tessellation_polyfill.value());
        auto const make = [&](compute_pipeline_description const& pass, cc::string_view name,
                              compute_pipeline_handle& into) -> cc::optional<cc::string>
        {
            auto made = try_create_compute_pipeline(pass);
            if (made.has_error())
                return cc::format("its tessellation polyfill's {} pass: {}", name, made.error());
            into = cc::move(made.value());
            return {};
        };
        for (auto const refusal :
             {make(passes.control, "control", state.control), make(passes.count, "count", state.count),
              make(passes.scan, "scan", state.scan), make(passes.generate, "generate", state.generate),
              make(passes.evaluation, "evaluation", state.evaluation)})
            if (refusal.has_value())
                return cc::error(refusal.value());
        tessellation = cc::move(state);
    }

    _ctx._stats.add(stat::pipelines_created);
    auto r = _ctx.try_create_raster_pipeline(desc, lifetime_scope::persistent);
    if (r.has_value())
    {
        impl::set_targets(*r.value(), target_set_of(desc), target_formats_of(desc), desc.mesh_shader.has_value());
        if (desc.geometry_mesh_corners > 0)
            impl::set_geometry_mesh_corners(*r.value(), desc.geometry_mesh_corners);
        add_debug_sites(_ctx._debug_windows, desc);
        impl::set_footprint(*r.value(), footprint_of(desc));
        if (desc.mesh_polyfill.has_value())
            impl::set_mesh_polyfill(*r.value(), state_of(desc.mesh_polyfill.value(), cc::move(halves)));
        if (tessellation.has_value())
            impl::set_tessellation_polyfill(*r.value(), cc::move(tessellation.value()));
    }
    return r;
}

cc::shared_async<compute_pipeline_handle> context_uncached_scope::create_compute_pipeline_async(
    compute_pipeline_description const& given)
{
    auto const desc = over_grid(_ctx, given);
    if (auto refusal = refusal_of(desc, _ctx.supported_features()); refusal.has_value())
        return cc::make_async_from_error<compute_pipeline_handle>(
            cc::async_error::make_error(cc::any_error(cc::move(refusal.value()))));
    _ctx._stats.add(stat::pipelines_created);
    add_debug_sites(_ctx._debug_windows, desc);
    return with_footprint(_ctx.create_compute_pipeline_async(desc, lifetime_scope::persistent), footprint_of(desc),
                          desc.shader.is_dispatched_by_threads);
}

cc::shared_async<raster_pipeline_handle> context_uncached_scope::create_raster_pipeline_async(
    raster_pipeline_description const& given)
{
    auto const desc = raster_half_of(_ctx, given);
    auto refusal = refusal_of(desc, _ctx.supported_features());
    if (!refusal.has_value())
        refusal = polyfill_refusal_of(desc, _ctx);
    if (!refusal.has_value())
        refusal = tessellation_refusal_of(desc, _ctx);
    if (!refusal.has_value())
        refusal = geometry_mesh_refusal_of(desc, _ctx);
    if (refusal.has_value())
        return cc::make_async_from_error<raster_pipeline_handle>(
            cc::async_error::make_error(cc::any_error(cc::move(refusal.value()))));

    // The compute half through the cache, which deduplicates it and starts it now, beside the raster half; the uncached
    // async build hands vulkan an empty shader here (bug-sg-uncached-async-compute-pipeline-empty-shader).
    auto compute = cc::shared_async<compute_pipeline_handle>();
    auto task = cc::shared_async<compute_pipeline_handle>();
    auto launched = cc::shared_async<compute_pipeline_handle>();
    auto scan = cc::shared_async<compute_pipeline_handle>();
    if (desc.mesh_polyfill.has_value())
    {
        compute = _ctx.cached.acquire_compute_pipeline(compute_half_of(_ctx, desc));
        auto const& p = desc.mesh_polyfill.value();
        if (p.task_shader.has_value())
            task = _ctx.cached.acquire_compute_pipeline(task_half_of(_ctx, desc, p.task_shader.value()));
        if (p.launched_shader.has_value())
            launched = _ctx.cached.acquire_compute_pipeline(compute_half_of(_ctx, desc, p.launched_shader.value(), true));
        if (p.task_shader.has_value() || p.launched_shader.has_value())
            scan = _ctx.cached.acquire_compute_pipeline(task_half_of(_ctx, desc, p.scan_shader));
    }

    // A backend may settle the build from a callback of its own, so the name is set once it has.
    _ctx._stats.add(stat::pipelines_created);
    auto built = _ctx.create_raster_pipeline_async(desc, lifetime_scope::persistent);
    add_debug_sites(_ctx._debug_windows, desc);
    auto result = named(cc::move(built), cc::string(target_set_of(desc)), target_formats_of(desc),
                        desc.mesh_shader.has_value(), footprint_of(desc), desc.geometry_mesh_corners);
    if (desc.tessellation_polyfill.has_value())
    {
        auto const passes = tessellation_passes_of(_ctx, desc);
        auto acquired = cc::fixed_array<cc::shared_async<compute_pipeline_handle>, 5>{
            _ctx.cached.acquire_compute_pipeline(passes.control), _ctx.cached.acquire_compute_pipeline(passes.count),
            _ctx.cached.acquire_compute_pipeline(passes.scan), _ctx.cached.acquire_compute_pipeline(passes.generate),
            _ctx.cached.acquire_compute_pipeline(passes.evaluation)};
        return with_tessellation(cc::move(result), cc::move(acquired),
                                 tessellation_state_of(desc.tessellation_polyfill.value()));
    }
    if (!desc.mesh_polyfill.has_value())
        return result;
    return with_polyfill(cc::move(result), cc::move(compute), cc::move(task), cc::move(launched), cc::move(scan),
                         desc.mesh_polyfill.value());
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
    if (auto missing = impl::find_missing_feature(_ctx.supported_features(), stages); missing.has_value())
        return cc::error(cc::move(missing.value()));
    if (auto reserved = impl::find_reserved_slot(stages); reserved.has_value())
        return cc::error(cc::move(reserved.value()));

    _ctx._stats.add(stat::pipelines_created);
    auto r = _ctx.try_create_raytracing_pipeline(desc, lifetime_scope::persistent);
    if (r.has_value())
    {
        add_debug_sites(_ctx._debug_windows, stages);
        impl::set_footprint(*r.value(), footprint_of(*desc.layout, stages));
        impl::set_hit_groups(*r.value(), desc.hit_shaders);
    }
    return r;
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
    // Here rather than per backend, so a bad count is the same error on every one and never reaches a constructor.
    if (desc.ray_count < 1)
        return cc::error(cc::format("raytracing_shader_table: ray_count must be >= 1, and is {}", desc.ray_count));
    return _ctx.try_create_raytracing_shader_table(desc, lifetime_scope::persistent);
}
} // namespace sg
