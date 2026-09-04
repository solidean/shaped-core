#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/layout_routine.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
/// The inline-constants block layout.hlsl declares, byte for byte.
struct layout_constants_gpu
{
    tg::vec4f uv_scale_bias_0 = tg::vec4f(1, 1, 0, 0);
    tg::vec4f uv_scale_bias_1 = tg::vec4f(1, 1, 0, 0);
    tg::vec4f tint = tg::vec4f(1, 1, 1, 1);
    tg::vec4f wipe = tg::vec4f(0.5f, 0, 0, 0);
    tg::vec4f separator_color = tg::vec4f(1, 1, 1, 1);
};

// "byte for byte" is now checkable: layout.hlsl's block reaches C++ as a generated mirror, so a field moved in
// the shader is a compile error here rather than a wrong sample.
// This struct stays because it carries the defaults a draw starts from, which a mirror does not.
static_assert(sizeof(layout_constants_gpu) == sizeof(sv::shaders::layout_constants));
static_assert(offsetof(sv::shaders::layout_constants, uv_scale_bias_0) == offsetof(layout_constants_gpu, uv_scale_bias_0));
static_assert(offsetof(sv::shaders::layout_constants, uv_scale_bias_1) == offsetof(layout_constants_gpu, uv_scale_bias_1));
static_assert(offsetof(sv::shaders::layout_constants, tint) == offsetof(layout_constants_gpu, tint));
static_assert(offsetof(sv::shaders::layout_constants, wipe) == offsetof(layout_constants_gpu, wipe));
static_assert(offsetof(sv::shaders::layout_constants, separator_color) == offsetof(layout_constants_gpu, separator_color));

/// A uv rect as the shader wants it: a scale and a bias applied to the covering triangle's [0,1] corner.
[[nodiscard]] tg::vec4f uv_scale_bias(tg::aabb2f const& uv)
{
    return tg::vec4f(uv.max[0] - uv.min[0], uv.max[1] - uv.min[1], uv.min[0], uv.min[1]);
}

/// Premultiplied source-over.
///
/// Premultiplied rather than straight because a view target is composited again by its own parent, and straight alpha
/// is not associative across that chain.
constexpr sg::blend_state over_blend
    = {.color = {.source = sg::blend_factor::one, .target = sg::blend_factor::one_minus_src_alpha},
       .alpha = {.source = sg::blend_factor::one, .target = sg::blend_factor::one_minus_src_alpha}};

/// A flat fill: a node's background, or one band of its border ring.
/// Both are one solid color and neither samples anything, which is why they share a pipeline.
[[nodiscard]] bool is_flat_fill(draw_kind kind)
{
    return kind == draw_kind::background || kind == draw_kind::border;
}

/// A flat fill always blends, so a translucent background composites and an invisible band cannot punch a hole in what
/// it frames.
[[nodiscard]] bool is_blended(layout_draw const& d)
{
    return is_flat_fill(d.kind) || d.blend == layer_blend::over;
}

[[nodiscard]] sg::texture_2d const* source_texture(draw_source const& s, plan_textures const& textures)
{
    auto const& pool = s.kind == draw_source_kind::trace ? textures.traces : textures.targets;
    return isize(s.index) < pool.size() ? &pool[s.index] : nullptr;
}
} // namespace

void layout_routine::init_declare(sg::context& ctx)
{
    auto vs = sv::shaders::layout.vertex.main_vs->acquire(ctx);
    auto border_ps = sv::shaders::layout.fragment.border_ps->acquire(ctx);
    auto view_ps = sv::shaders::layout.fragment.view_ps->acquire(ctx);
    auto wipe_ps = sv::shaders::layout.fragment.wipe_ps->acquire(ctx);

    (void)cc::try_async_blocking_get(vs);
    (void)cc::try_async_blocking_get(border_ps);
    (void)cc::try_async_blocking_get(view_ps);
    (void)cc::try_async_blocking_get(wipe_ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_border = border_ps->try_value();
    auto const* const compiled_view = view_ps->try_value();
    auto const* const compiled_wipe = wipe_ps->try_value();

    // A broken edit: (re)bind a callback that fails, so init still clears every pipeline built against the old layout
    // and execute no-ops until the next reload compiles.
    if (compiled_vs == nullptr || compiled_border == nullptr || compiled_view == nullptr || compiled_wipe == nullptr)
    {
        _group_layout = nullptr;
        _pipelines.init(ctx,
                        [](sg::context&, impl::layout_pipeline_key) -> sg::async_raster_pipeline
                        {
                            return cc::make_async_from_error<sg::raster_pipeline_handle>(
                                cc::async_error::make_error(cc::any_error("layout shaders did not compile")));
                        });
        return;
    }

    // The group is what layout.hlsl declared, so it serves every kind whatever a stage happens to reference:
    // a one-source draw simply binds its primary twice.
    // Nothing has to reason about which stage to reflect either — the constants block is not a group member.
    _group_layout = shaders::layout_bindings::group::acquire_layout(ctx);

    auto const* const constants_binding = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled_vs->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    // A vertex stage that reflects no constants block cannot be driven, but this is a shader edit like any other:
    // fail the build callback so execute no-ops, rather than taking the process down on the default preset.
    if (constants_binding == nullptr)
    {
        _group_layout = nullptr;
        _pipelines.init(ctx,
                        [](sg::context&, impl::layout_pipeline_key) -> sg::async_raster_pipeline
                        {
                            return cc::make_async_from_error<sg::raster_pipeline_handle>(cc::async_error::make_error(
                                cc::any_error("layout.hlsl declares no layout_constants cbuffer")));
                        });
        return;
    }

    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}, .inline_constants = *constants_binding});

    _pipelines.init(
        ctx,
        [layout = pipeline_layout, vertex_shader = *compiled_vs, border = *compiled_border, view = *compiled_view,
         wipe = *compiled_wipe](sg::context& c, impl::layout_pipeline_key key) -> sg::async_raster_pipeline
        {
            // The border stage is the flat-color one, so a background renders through it too.
            auto const& fragment_shader = is_flat_fill(key.kind) ? border : key.kind == draw_kind::wipe ? wipe : view;

            auto target = sg::color_target_state{.format = key.format};
            if (key.blended)
                target.blend = over_blend;

            auto const desc = sg::raster_pipeline_description{
                .layout = layout,
                .vertex_shader = vertex_shader,
                .fragment_shader = fragment_shader,
                .topology = sg::primitive_topology::triangle_list, // no vertex input — SV_VertexID
                .rasterization = {.cull = sg::cull_mode::none},
                .color_targets = {target},
            };
            return c.cached.acquire_raster_pipeline(desc);
        });
}

void layout_routine::execute(sg::rendering_scope& scope,
                             window_id window,
                             cc::span<layout_draw const> draws,
                             plan_textures const& textures)
{
    (void)window; // keyed on today, and the seam a per-window color space plugs into

    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "a layout must be drawn into a scope with a color target");
    auto const format = scope.color_formats()[0];

    auto const& self = acquire(cmd);
    auto& ctx = cmd.context();

    for (auto const& d : draws)
    {
        auto const w = d.dst_rect.max[0] - d.dst_rect.min[0];
        auto const h = d.dst_rect.max[1] - d.dst_rect.min[1];
        if (w <= 0 || h <= 0)
            continue; // a collapsed cell draws nothing rather than a degenerate viewport

        // Fallible rather than throwing: this runs inside the caller's rendering scope, and an exception unwinding out
        // of there would leave their command list unsubmitted.
        auto const pipeline = self._pipelines.try_acquire({.format = format, .kind = d.kind, .blended = is_blended(d)});
        if (pipeline.has_error() || pipeline.value() == nullptr)
            return;

        auto constants = layout_constants_gpu{};
        constants.tint = tg::vec4f(d.opacity, d.opacity, d.opacity, d.opacity); // premultiplied, so the color scales too

        auto group = std::shared_ptr<sg::binding_group const>();
        if (is_flat_fill(d.kind))
        {
            constants.tint = d.color;

            // Every pipeline shares one group layout, so even a flat fill binds the two source slots.
            // It samples neither; binding the same texture twice is cheaper than a second layout.
            if (textures.targets.empty())
                continue;
            auto built = shaders::layout_bindings::group{
                .source_0 = textures.targets[0].as_readonly_view(),
                .source_1 = textures.targets[0].as_readonly_view(),
                .source_sampler
                = {}}.create(ctx, sg::lifetime_scope::transient);
            if (built.has_error())
                continue;
            group = cc::move(built.value());
        }
        else
        {
            auto const* const primary = source_texture(d.primary, textures);
            if (primary == nullptr)
                continue;

            auto const* const secondary = d.kind == draw_kind::wipe ? source_texture(d.secondary, textures) : primary;
            if (secondary == nullptr)
                continue;

            constants.uv_scale_bias_0 = uv_scale_bias(d.primary.uv);
            constants.uv_scale_bias_1 = uv_scale_bias(d.secondary.uv);
            constants.wipe = tg::vec4f(d.post.split, d.post.horizontal ? 0.0f : 1.0f,
                                       w > 0 ? 0.5f * float(d.post.separator_width) / float(w) : 0.0f, 0.0f);
            constants.separator_color = d.post.separator_color;

            auto const filter
                = d.sampler == sampler_mode::nearest ? sg::sampler_filter::nearest : sg::sampler_filter::linear;
            auto built
                = shaders::layout_bindings::group{.source_0 = primary->as_readonly_view(),
                                                  .source_1 = secondary->as_readonly_view(),
                                                  .source_sampler = {.min_filter = filter,
                                                                     .mag_filter = filter,
                                                                     .mip_filter = sg::sampler_filter::nearest,
                                                                     .address_u = sg::sampler_address_mode::clamp_edge,
                                                                     .address_v = sg::sampler_address_mode::clamp_edge}}
                      .create(ctx, sg::lifetime_scope::transient);
            if (built.has_error())
                continue;
            group = cc::move(built.value());
        }

        scope.set_viewport(
            {.offset = tg::pos2f(f32(d.dst_rect.min[0]), f32(d.dst_rect.min[1])), .size = tg::vec2f(f32(w), f32(h))});
        scope.set_scissor(d.dst_rect);
        scope.bind_pipeline(*pipeline.value());
        shaders::layout_bindings::group::bind(scope, *group);
        scope.set_inline_constants(cc::span<layout_constants_gpu const>(&constants, 1).as_bytes(), {});
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
}
} // namespace sv
