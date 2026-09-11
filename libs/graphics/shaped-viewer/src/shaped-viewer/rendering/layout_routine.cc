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

    // Cleared first, so a shader that was never good leaves nothing behind to draw with.
    _group_layout = nullptr;
    for (auto& by_kind : _pipelines)
        for (auto& p : by_kind)
            p = nullptr;

    if (compiled_vs == nullptr || compiled_border == nullptr || compiled_view == nullptr || compiled_wipe == nullptr)
    {
        fail_init();
        return;
    }

    // Group 0 comes from the *wipe* fragment stage, which is the only one binding both sources — so one layout serves
    // every kind, and a one-source draw simply binds its primary twice.
    // Built from a fragment stage alone, which is what keeps the vertex stage's b0 out of it: inline constants must be
    // excluded from every group layout (see pipeline_layout.hh).
    _group_layout = ctx.cached.acquire_binding_group_layout(compiled_wipe->bindings);

    auto const* const constants_binding = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled_vs->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    // A vertex stage that reflects no constants block cannot be driven, but this is a shader problem like any other:
    // report it as a failed init rather than taking the process down on the default preset.
    if (constants_binding == nullptr)
    {
        _group_layout = nullptr;
        fail_init();
        return;
    }

    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}, .inline_constants = *constants_binding});

    // Every pipeline this format needs, built here rather than on demand: a draw happens inside the caller's open
    // rendering scope, and that is where nothing may wait.
    //
    // Six of the eight (kind, blended) slots are reachable.
    // A flat fill is always blended -- is_blended returns true for one unconditionally -- so background and border
    // have no unblended form, and those two slots stay null.
    auto pending = cc::vector<sg::async_raster_pipeline>();
    for (auto kind_index = 0; kind_index < k_draw_kinds; ++kind_index)
    {
        auto const kind = draw_kind(kind_index);
        for (auto blended = 0; blended < 2; ++blended)
        {
            if (is_flat_fill(kind) && blended == 0)
                continue; // unreachable: a flat fill always blends

            // The border stage is the flat-color one, so a background renders through it too.
            auto const& fragment_shader = is_flat_fill(kind)      ? *compiled_border
                                        : kind == draw_kind::wipe ? *compiled_wipe
                                                                  : *compiled_view;

            auto target = sg::color_target_state{.format = params()};
            if (blended != 0)
                target.blend = over_blend;

            auto node = ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
                .layout = pipeline_layout,
                .vertex_shader = *compiled_vs,
                .fragment_shader = fragment_shader,
                .topology = sg::primitive_topology::triangle_list, // no vertex input — SV_VertexID
                .rasterization = {.cull = sg::cull_mode::none},
                .color_targets = {target},
            });
            pending.push_back(node);
        }
    }

    // Started together, then collected: the builds overlap rather than running one after another.
    // These waits are what become a single co_await once init is a coroutine.
    auto at = isize(0);
    for (auto kind_index = 0; kind_index < k_draw_kinds; ++kind_index)
    {
        auto const kind = draw_kind(kind_index);
        for (auto blended = 0; blended < 2; ++blended)
        {
            if (is_flat_fill(kind) && blended == 0)
                continue;

            auto const built = cc::try_async_blocking_get(pending[at++]);
            if (built.has_error())
            {
                fail_init(); // one pipeline missing makes the whole routine unusable, so say so once
                return;
            }
            _pipelines[kind_index][blended] = built.value();
        }
    }
}

sg::routine_outcome layout_routine::execute(sg::rendering_scope& scope,
                                            window_id window,
                                            cc::span<layout_draw const> draws,
                                            plan_textures const& textures)
{
    (void)window; // keyed on today, and the seam a per-window color space plugs into

    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "a layout must be drawn into a scope with a color target");
    auto const format = scope.color_formats()[0];

    // The target's format picks the instance, and is only knowable once the caller's scope is open.
    auto const self = try_acquire(cmd, format);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();

    for (auto const& d : draws)
    {
        auto const w = d.dst_rect.max[0] - d.dst_rect.min[0];
        auto const h = d.dst_rect.max[1] - d.dst_rect.min[1];
        if (w <= 0 || h <= 0)
            continue; // a collapsed cell draws nothing rather than a degenerate viewport

        // An index rather than a lookup: every pipeline this format needs was built during init, so there is nothing
        // here that could still be building.
        auto const& pipeline = self->_pipelines[int(d.kind)][is_blended(d) ? 1 : 0];
        CC_ASSERT(pipeline != nullptr, "a layout draw reached a (kind, blend) combination init did not build");

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
            group = ctx.transient.create_binding_group(
                self->_group_layout,
                {{.name = "source_0", .view = textures.targets[0].as_readonly_view()},
                 {.name = "source_1", .view = textures.targets[0].as_readonly_view()}},
                {{.name = "source_sampler", .sampler = {}}});
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
            group
                = ctx.transient.create_binding_group(self->_group_layout,
                                                     {{.name = "source_0", .view = primary->as_readonly_view()},
                                                      {.name = "source_1", .view = secondary->as_readonly_view()}},
                                                     {{.name = "source_sampler",
                                                       .sampler = {.min_filter = filter,
                                                                   .mag_filter = filter,
                                                                   .mip_filter = sg::sampler_filter::nearest,
                                                                   .address_u = sg::sampler_address_mode::clamp_edge,
                                                                   .address_v = sg::sampler_address_mode::clamp_edge}}});
        }

        scope.set_viewport(
            {.offset = tg::pos2f(f32(d.dst_rect.min[0]), f32(d.dst_rect.min[1])), .size = tg::vec2f(f32(w), f32(h))});
        scope.set_scissor(d.dst_rect);
        scope.bind_pipeline(*pipeline);
        scope.bind_group(0, *group);
        scope.set_inline_constants(cc::span<layout_constants_gpu const>(&constants, 1).as_bytes(), {});
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    return sg::routine_outcome::executed;
}
} // namespace sv
