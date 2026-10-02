#include <clean-core/common/asserts.hh>
#include <clean-core/fwd.hh> // offsetof
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raster.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <sr_sgl_shaders.hh>

// slug_quads.sgl's instance stream reaches C++ as a generated struct; the public one has to be it byte for byte.
using generated_instance = sr::sgl_shaders::slug_vertex::per_instance;
static_assert(sizeof(sr::slug_instance) == sizeof(generated_instance),
              "slug_instance is not slug_quads.sgl's instance stream");
static_assert(offsetof(sr::slug_instance, em_to_object) == offsetof(generated_instance, em_to_object),
              "em_to_object moved in slug_quads.sgl");
static_assert(offsetof(sr::slug_instance, origin) == offsetof(generated_instance, origin),
              "origin moved in slug_quads.sgl");
static_assert(offsetof(sr::slug_instance, em_bounds) == offsetof(generated_instance, em_bounds),
              "em_bounds moved in slug_quads.sgl");
static_assert(offsetof(sr::slug_instance, banding) == offsetof(generated_instance, banding),
              "banding moved in slug_quads.sgl");
static_assert(offsetof(sr::slug_instance, glyph_location) == offsetof(generated_instance, glyph),
              "glyph moved in slug_quads.sgl");
static_assert(offsetof(sr::slug_instance, color) == offsetof(generated_instance, color), "color moved in slug_quads.sgl");
static_assert(sizeof(tg::vec2f) == sizeof(sr::sgl_shaders::slug_vertex::per_vertex), "the corner stream is one float2");

namespace sr
{
cc::shared_async<cc::unit> slug_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    auto const vs = sgl_shaders::slug_quads.main_vs->acquire(ctx);
    auto const ps = sgl_shaders::slug_quads.main_ps->acquire(ctx);
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    _group_layout = nullptr;
    _pipeline = {};
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        fail_init();
        co_return;
    }

    if (_corners.raw() == nullptr)
    {
        tg::vec2f const corners[]
            = {tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(1, 1), tg::vec2f(0, 0), tg::vec2f(1, 1), tg::vec2f(0, 1)};
        _corners = ctx.persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    }

    _group_layout = ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>();
    auto const layout = ctx.cached.acquire_pipeline_layout<sgl_modules::slug::tables, sgl_shaders::slug_draw>();

    // Both windings reach the screen: a shape's axes may flip it, and a quad is never seen from behind on purpose.
    auto desc = sg::raster_pipeline_description{
        .layout = layout,
        .vertex_shader = *compiled_vs,
        .fragment_shader = *compiled_ps,
        .vertex_input = sgl_shaders::slug_vertex::layout(),
        .topology = sg::primitive_topology::triangle_list,
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = {{.format = params().color, .blend = sg::blend_premultiplied_alpha}}};
    if (params().depth != sg::pixel_format::undefined)
    {
        // Tested and never written: shapes on one surface then layer in the order they were drawn.
        desc.depth_stencil_format = params().depth;
        desc.depth_stencil = {.depth_test = true, .depth_write = false, .depth_compare = sg::compare_op::less_equal};
    }
    _pipeline = ctx.cached.acquire_raster_pipeline(desc);
    co_await cc::async_settled(_pipeline);
    co_return;
}

slug_routine::prepared_shapes slug_routine::prepare(sg::command_list& cmd,
                                                    slug_atlas& atlas,
                                                    cc::span<slug_instance const> instances)
{
    atlas.prepare(cmd);

    auto shapes = prepared_shapes{.command_list = &cmd};
    if (instances.empty())
        return shapes;

    shapes.instances = cmd.context().transient.create_buffer<slug_instance>(
        instances.size(), sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.data_to_buffer(shapes.instances, instances);
    return shapes;
}

sg::routine_outcome slug_routine::execute(sg::rendering_scope& scope,
                                          slug_atlas const& atlas,
                                          prepared_shapes const& shapes,
                                          slug_view const& view)
{
    CC_ASSERT(shapes.command_list == &scope.command_list(), "these shapes were prepared on another command list");
    if (shapes.instances.raw() == nullptr)
        return sg::routine_outcome::executed; // nothing to draw is not a refusal
    return execute(scope, atlas, shapes.instances, 0, shapes.instances.element_count(), view);
}

sg::routine_outcome slug_routine::execute(sg::rendering_scope& scope,
                                          slug_atlas const& atlas,
                                          sg::buffer<slug_instance> const& instances,
                                          isize first,
                                          isize count,
                                          slug_view const& view)
{
    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "slug shapes must be drawn into a scope with a color target");
    auto const depth = scope.depth_format();
    auto const key = slug_pipeline_key{.color = scope.color_formats()[0],
                                       .depth = depth.has_value() ? depth.value() : sg::pixel_format::undefined};

    auto self = try_acquire_exclusive(cmd, key);
    if (!self.is_ready())
        return sg::routine_outcome::declined;

    auto const* const pipeline = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (pipeline == nullptr || *pipeline == nullptr)
        return sg::routine_outcome::declined;
    if (count == 0)
        return sg::routine_outcome::executed;
    CC_ASSERT(atlas.curve_texture().raw() != nullptr, "the atlas was never prepared; call slug_routine::prepare or "
                                                      "atlas.prepare first");
    CC_ASSERT(!atlas.has_pending_upload(),
              "the atlas gained shapes since its last prepare, so the GPU cannot see them; "
              "prepare it before the rendering scope opens");

    auto& ctx = cmd.context();
    auto const group = ctx.transient.create_binding_group(
        cmd, self->_group_layout,
        sgl_modules::slug::tables{.curves = atlas.curve_texture().as_texture_view(),
                                  .bands = atlas.band_texture().as_texture_view()});

    auto const size = scope.render_target_size();
    auto const& m = view.object_to_clip;
    auto const row = [&](int r) { return tg::vec4f(m[0, r], m[1, r], m[2, r], m[3, r]); };

    scope.bind_pipeline(**pipeline);
    scope.bind_group(0, *group);
    scope.bind_vertex_buffers({self->_corners.as_vertex_buffer(), instances.as_vertex_buffer()});
    scope.set_inline_constants(sgl_shaders::slug_draw{.row0 = row(0),
                                                      .row1 = row(1),
                                                      .row2 = row(2),
                                                      .row3 = row(3),
                                                      .viewport = tg::vec2f(f32(size[0]), f32(size[1])),
                                                      .depth_bias = view.depth_bias,
                                                      .weight_boost = view.weight_boost ? 1 : 0}
                                   .to_block());
    scope.draw({.vertex_range = {.offset = 0, .size = 6}, .instance_range = {.offset = first, .size = count}});
    return sg::routine_outcome::executed;
}

u32 pack_rgba8(tg::vec4f c)
{
    auto const channel = [](f32 v) { return u32(cc::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(c[0]) | (channel(c[1]) << 8) | (channel(c[2]) << 16) | (channel(c[3]) << 24);
}

slug_instance make_slug_instance(slug_shape_ref const& shape,
                                 tg::pos2f origin,
                                 tg::vec2f x_axis,
                                 tg::vec2f y_axis,
                                 tg::vec4f srgb_color)
{
    CC_ASSERT(shape.is_drawable, "an empty shape has nothing to draw; skip refs whose is_drawable is false");

    // Stored em is the outline less stored_origin, scaled by em_scale: one stored unit is 1 / em_scale of an outline
    // unit, and stored (0, 0) is where stored_origin lands on the axes.
    auto const inv = 1.0f / shape.em_scale;
    auto const at = origin + x_axis * shape.stored_origin[0] + y_axis * shape.stored_origin[1];
    return {.em_to_object = tg::vec4f(x_axis[0] * inv, x_axis[1] * inv, y_axis[0] * inv, y_axis[1] * inv),
            .origin = tg::vec2f(at[0], at[1]),
            .em_bounds
            = tg::vec4f(shape.em_bounds.min[0], shape.em_bounds.min[1], shape.em_bounds.max[0], shape.em_bounds.max[1]),
            .banding = shape.banding,
            .glyph_location = shape.glyph_location,
            .band_info = shape.band_info,
            .color = pack_rgba8(srgb_color)};
}
} // namespace sr
