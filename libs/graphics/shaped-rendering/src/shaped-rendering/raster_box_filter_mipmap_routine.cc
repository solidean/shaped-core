#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/raster_box_filter_mipmap_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
cc::shared_async<cc::unit> raster_box_filter_mipmap_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    auto const vs = sr::shaders::raster_box_filter_mipmap.vertex.main_vs->acquire(ctx);
    auto const ps = sr::shaders::raster_box_filter_mipmap.fragment.main_ps->acquire(ctx);

    // Settled rather than awaited for the value: a shader that did not compile is this routine's verdict to report,
    // not an error to propagate — fail_init says so in the vocabulary a caller already branches on.
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    // Null only when a shader was never good: a failed reload keeps the last one that compiled and never gets here.
    _group_layout = nullptr;
    _pipeline = {};
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        fail_init(); // not pending: this will not come good until a reload, and a caller should be able to tell
        co_return;
    }

    // The fragment stage carries the one binding: gSource (t0), the single-mip view of the level being read.
    // No sampler — the filter loads its four texels rather than sampling, which is what makes the tap positions
    // exact on an odd-sized level.
    _group_layout = ctx.cached.acquire_binding_group_layout(compiled_ps->bindings);

    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});

    _pipeline = ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = pipeline_layout,
        .vertex_shader = *compiled_vs,
        .fragment_shader = *compiled_ps,
        .topology = sg::primitive_topology::triangle_list, // no vertex input — SV_VertexID
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = {{.format = params()}},
    });

    // Awaited HERE rather than polled in execute, which is the whole point of the split: `ready` means ready, so a
    // caller that got the routine handed to it never sees it decline for a few frames while a pipeline finishes.
    co_await cc::async_settled(_pipeline);
    co_return;
}

int raster_box_filter_mipmap_routine::level_count(sg::texture_2d const& texture, int first_level)
{
    CC_ASSERT(first_level >= 1, "level 0 is the source of the chain and is never generated");
    auto const levels = texture.mip_levels();
    return first_level >= levels ? 0 : levels - first_level;
}

sg::routine_outcome raster_box_filter_mipmap_routine::execute(sg::command_list& cmd,
                                                              sg::texture_2d const& texture,
                                                              int first_level)
{
    if (level_count(texture, first_level) == 0)
        return sg::routine_outcome::executed; // nothing to generate is not a refusal

    // The texture's format picks the instance, and it is only knowable here.
    auto const self = try_acquire(cmd, texture.format());
    if (!self.is_ready())
        return sg::routine_outcome::declined;

    // Polled rather than waited on: this records into the caller's command list, so nothing here may block, and a
    // throw would leave that list unsubmitted.
    auto const* const built = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (built == nullptr || *built == nullptr)
        return sg::routine_outcome::declined;

    auto& ctx = cmd.context();

    auto const levels = texture.mip_levels();
    for (auto level = first_level; level < levels; ++level)
    {
        auto const group = ctx.transient.create_binding_group(
            self->_group_layout,
            {{.name = "gSource", .view = texture.as_readonly_view({.mips = {.start = level - 1, .count = 1}})}});

        // Discarded rather than preserved: the pass covers the whole level, so loading what is there costs
        // bandwidth for texels every one of which is about to be overwritten.
        //
        // One scope per level, closed before the next opens.
        // That is not tidiness: the scope end is what releases the output-merger binding, and level N could not
        // transition to a sampled read while it is still bound as a target.
        auto scope = cmd.raster.render_to({.color_targets = {texture.as_render_target_view({.mip = level}).discarded()}});

        scope.bind_pipeline(**built);
        scope.bind_group(0, *group);
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    return sg::routine_outcome::executed;
}
} // namespace sr
