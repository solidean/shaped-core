#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/blit_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
void blit_routine::init_declare(sg::context& ctx)
{
    auto vs = sr::shaders::blit.vertex.main_vs->acquire(ctx);
    auto ps = sr::shaders::blit.fragment.main_ps->acquire(ctx);

    (void)cc::try_async_blocking_get(vs);
    (void)cc::try_async_blocking_get(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    // A broken edit: (re)bind a callback that fails, so init still clears every pipeline built against the old layout and execute no-ops until the next reload compiles.
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        _group_layout = nullptr;
        _pipelines.init(ctx, [](sg::context&, sg::pixel_format) -> cc::result<sg::raster_pipeline_handle>
                        { return cc::error(cc::any_error("blit shaders did not compile")); });
        return;
    }

    // The fragment stage carries both bindings: source_texture (t0) and the dynamic linear_sampler (s0).
    _group_layout = ctx.cached.acquire_binding_group_layout(compiled_ps->bindings);

    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});

    // The callback captures the layout + shaders.
    // Init clears every pipeline built against the previous ones.
    _pipelines.init(ctx,
                    [layout = pipeline_layout, vertex_shader = *compiled_vs, fragment_shader = *compiled_ps](
                        sg::context& c, sg::pixel_format format) -> cc::result<sg::raster_pipeline_handle>
                    {
                        auto const desc = sg::raster_pipeline_description{
                            .layout = layout,
                            .vertex_shader = vertex_shader,
                            .fragment_shader = fragment_shader,
                            .topology = sg::primitive_topology::triangle_list, // no vertex input — SV_VertexID
                            .rasterization = {.cull = sg::cull_mode::none},
                            .color_targets = {{.format = format}},
                        };
                        return build_cached_raster_pipeline(c, desc);
                    });
}

void blit_routine::execute(sg::rendering_scope& scope, sg::texture_2d const& src)
{
    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "blit must be drawn into a scope with a color target");
    auto const format = scope.color_formats()[0];

    auto const& self = acquire(cmd);
    auto& ctx = cmd.context();

    // Fallible rather than throwing: execute() runs inside the caller's rendering scope.
    // An exception unwinding out of there would leave their command list unsubmitted.
    auto const pipeline = self._pipelines.try_acquire(format);
    if (pipeline.has_error() || pipeline.value() == nullptr)
        return; // shaders did not compile, or this format's pipeline failed to build

    auto const group = ctx.transient.create_binding_group(
        self._group_layout, {{.name = "source_texture", .view = src.as_readonly_view()}},
        {{.name = "linear_sampler",
          .sampler = {.min_filter = sg::sampler_filter::linear,
                      .mag_filter = sg::sampler_filter::linear,
                      .mip_filter = sg::sampler_filter::nearest,
                      .address_u = sg::sampler_address_mode::clamp_edge,
                      .address_v = sg::sampler_address_mode::clamp_edge}}});

    scope.bind_pipeline(*pipeline.value());
    scope.bind_group(0, *group);
    scope.draw({.vertex_range = {.offset = 0, .size = 3}});
}
} // namespace sr
