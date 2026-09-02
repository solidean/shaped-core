#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/raster_box_filter_mipmap_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
void raster_box_filter_mipmap_routine::init_declare(sg::context& ctx)
{
    auto vs = sr::shaders::raster_box_filter_mipmap.vertex.main_vs->acquire(ctx);
    auto ps = sr::shaders::raster_box_filter_mipmap.fragment.main_ps->acquire(ctx);

    (void)cc::try_async_blocking_get(vs);
    (void)cc::try_async_blocking_get(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    // A broken edit: (re)bind a callback that fails, so init still clears every pipeline built against the old
    // layout and execute no-ops until the next reload compiles.
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        _group_layout = nullptr;
        _pipelines.init(ctx,
                        [](sg::context&, sg::pixel_format) -> sg::async_raster_pipeline
                        {
                            return cc::make_async_from_error<sg::raster_pipeline_handle>(
                                cc::async_error::make_error(cc::any_error("raster mipmap shaders did not compile")));
                        });
        return;
    }

    // The fragment stage carries the one binding: gSource (t0), the single-mip view of the level being read.
    // No sampler — the filter loads its four texels rather than sampling, which is what makes the tap positions
    // exact on an odd-sized level.
    _group_layout = ctx.cached.acquire_binding_group_layout(compiled_ps->bindings);

    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});

    _pipelines.init(ctx,
                    [layout = pipeline_layout, vertex_shader = *compiled_vs, fragment_shader = *compiled_ps](
                        sg::context& c, sg::pixel_format format) -> sg::async_raster_pipeline
                    {
                        auto const desc = sg::raster_pipeline_description{
                            .layout = layout,
                            .vertex_shader = vertex_shader,
                            .fragment_shader = fragment_shader,
                            .topology = sg::primitive_topology::triangle_list, // no vertex input — SV_VertexID
                            .rasterization = {.cull = sg::cull_mode::none},
                            .color_targets = {{.format = format}},
                        };
                        return c.cached.acquire_raster_pipeline(desc);
                    });
}

int raster_box_filter_mipmap_routine::level_count(sg::texture_2d const& texture, int first_level)
{
    CC_ASSERT(first_level >= 1, "level 0 is the source of the chain and is never generated");
    auto const levels = texture.mip_levels();
    return first_level >= levels ? 0 : levels - first_level;
}

void raster_box_filter_mipmap_routine::execute(sg::command_list& cmd, sg::texture_2d const& texture, int first_level)
{
    if (level_count(texture, first_level) == 0)
        return;

    auto const& self = acquire(cmd);
    auto& ctx = cmd.context();

    // Fallible rather than throwing: this runs inside the caller's command list, and an exception unwinding out
    // of here would leave it unsubmitted.
    auto const pipeline = self._pipelines.try_acquire(texture.format());
    if (pipeline.has_error() || pipeline.value() == nullptr)
        return; // the shaders did not compile, or this format's pipeline failed to build

    auto const levels = texture.mip_levels();
    for (auto level = first_level; level < levels; ++level)
    {
        auto const group = ctx.transient.create_binding_group(
            self._group_layout,
            {{.name = "gSource", .view = texture.as_readonly_view({.mips = {.start = level - 1, .count = 1}})}});

        // Discarded rather than preserved: the pass covers the whole level, so loading what is there costs
        // bandwidth for texels every one of which is about to be overwritten.
        //
        // One scope per level, closed before the next opens.
        // That is not tidiness: the scope end is what releases the output-merger binding, and level N could not
        // transition to a sampled read while it is still bound as a target.
        auto scope = cmd.raster.render_to({.color_targets = {texture.as_render_target_view({.mip = level}).discarded()}});

        scope.bind_pipeline(*pipeline.value());
        scope.bind_group(0, *group);
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
}
} // namespace sr
