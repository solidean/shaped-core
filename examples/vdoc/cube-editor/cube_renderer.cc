#include "cube_renderer.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/thread/async.hh>
#include <cube_shaders.hh>

using namespace cc::primitive_defines;

namespace
{
/// The unit cube, expanded so every face carries its own normal.
/// Wound clockwise as seen from outside, which is what the pipeline's front_face below expects.
struct cube_vertex
{
    tg::pos3f position;
    tg::vec3f normal;
};

constexpr int cube_vertex_count = 24;
constexpr int cube_index_count = 36;

[[nodiscard]] cc::array<cube_vertex> build_cube_mesh()
{
    tg::vec3f const normals[] = {tg::vec3f(0, 0, -1), tg::vec3f(0, 0, 1), tg::vec3f(-1, 0, 0),
                                 tg::vec3f(1, 0, 0),  tg::vec3f(0, -1, 0), tg::vec3f(0, 1, 0)};

    auto out = cc::array<cube_vertex>::create_defaulted(cube_vertex_count);
    for (auto face = 0; face < 6; ++face)
    {
        auto const n = normals[face];
        // Two in-plane axes for this face, picked so the winding stays consistent across all six.
        auto const u = tg::vec3f(n[1], n[2], n[0]);
        auto const v = tg::dual(tg::cross(n, u));

        for (auto corner = 0; corner < 4; ++corner)
        {
            auto const su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            auto const sv = (corner >= 2) ? 1.0f : -1.0f;
            out[face * 4 + corner] = {.position = tg::pos3f::zero + (n + u * su + v * sv), .normal = n};
        }
    }
    return out;
}

[[nodiscard]] cc::array<u16> build_cube_indices()
{
    auto out = cc::array<u16>::create_defaulted(cube_index_count);
    for (auto face = 0; face < 6; ++face)
    {
        auto const base = u16(face * 4);
        u16 const quad[] = {0, 1, 2, 0, 2, 3};
        for (auto i = 0; i < 6; ++i)
            out[face * 6 + i] = u16(base + quad[i]);
    }
    return out;
}
} // namespace

template <>
struct sg::vertex_layout_of<cube_vertex>
{
    static sg::vertex_type_layout get()
    {
        return {.stride = sizeof(cube_vertex),
                .attributes = {
                    {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(cube_vertex, position)},
                    {.semantic = "NORMAL", .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(cube_vertex, normal)},
                }};
    }
};

template <>
struct sg::vertex_layout_of<cube_editor::cube_instance>
{
    static sg::vertex_type_layout get()
    {
        using instance = cube_editor::cube_instance;
        return {.stride = sizeof(instance),
                .per_instance = true, // one step per cube, not per vertex — this is the whole point of the second slot
                .attributes = {
                    {.semantic = "TEXCOORD", .semantic_index = 0, .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(instance, center)},
                    {.semantic = "TEXCOORD", .semantic_index = 1, .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(instance, half_extent)},
                    {.semantic = "TEXCOORD", .semantic_index = 2, .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(instance, color)},
                    {.semantic = "TEXCOORD", .semantic_index = 3, .format = sg::vertex_attribute_format::f32, .offset = offsetof(instance, highlight)},
                }};
    }
};

namespace cube_editor
{
cc::vector<cube_instance> collect_instances(vdoc::document const& doc, vdoc::entity_id selected)
{
    auto out = cc::vector<cube_instance>();
    doc.each<placement, style>(
        [&](vdoc::entity_id entity, placement const& p, style const& s)
        {
            out.push_back({.center = p.center,
                           .half_extent = tg::vec3f(p.half_extent, p.half_extent, p.half_extent),
                           .color = s.color,
                           .highlight = entity == selected ? 1.0f : 0.0f});
        });
    return out;
}

cc::result<cc::unique_ptr<renderer>> renderer::create(sg::context& ctx, slib::shader_library& lib)
{
    (void)lib; // the package was mounted at startup; the handles below reach it through the library

    auto vs = shaders::cube.vertex.main_vs->acquire(ctx);
    auto ps = shaders::cube.fragment.main_ps->acquire(ctx);

    // Driven inline rather than on a pool: this runs once at startup, and the caller owes nothing.
    (void)cc::try_async_blocking_get_singlethreaded(vs);
    (void)cc::try_async_blocking_get_singlethreaded(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();
    if (compiled_vs == nullptr || compiled_ps == nullptr)
        return cc::error(cc::any_error("cube.hlsl did not compile"));

    // The only binding is the vertex stage's 64-byte view-projection block, and it rides as inline constants —
    // so there are no binding groups at all, which is why nothing here builds one.
    auto const* const constants = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled_vs->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    if (constants == nullptr)
        return cc::error(cc::any_error("cube.hlsl must declare the cube_constants cbuffer"));

    auto const layout = ctx.cached.acquire_pipeline_layout({.inline_constants = *constants});

    auto out = cc::make_unique<renderer>();
    out->_pipelines.init(ctx,
                        [layout, vertex_shader = *compiled_vs, fragment_shader = *compiled_ps](
                            sg::context& c, sg::pixel_format format) -> cc::result<sg::raster_pipeline_handle>
                        {
                            return c.uncached.try_create_raster_pipeline(
                                {.layout = layout,
                                 .vertex_shader = vertex_shader,
                                 .fragment_shader = fragment_shader,
                                 .vertex_input = sg::vertex_input_layout::create<cube_vertex, cube_instance>(),
                                 .rasterization = {.cull = sg::cull_mode::back},
                                 // Both default to OFF, and solid geometry needs both — a cube drawn without them
                                 // shows whichever face happened to be recorded last.
                                 .depth_stencil = {.depth_test = true, .depth_write = true},
                                 .color_targets = {{.format = format}},
                                 .depth_stencil_format = sg::pixel_format::depth32_float});
                        });
    return out;
}

void renderer::draw(sg::rendering_scope& scope, vdoc::document const& doc, tg::mat4f const& view_projection, vdoc::entity_id selected)
{
    auto& cmd = scope.command_list();
    auto& ctx = cmd.context();

    // try_acquire, never acquire: an exception unwinding out of an open scope would leave the list unsubmitted.
    auto const pipeline = _pipelines.try_acquire(scope.color_formats()[0]);
    if (pipeline.has_error() || pipeline.value() == nullptr)
        return;

    _instances = collect_instances(doc, selected);
    if (_instances.empty())
        return;

    // Transient: allocated from the per-epoch bump heap and recycled at advance_epoch, which is the right lifetime
    // for anything rebuilt every frame. A real renderer would keep the static mesh persistent; at 24 vertices the
    // difference is not worth the extra lifetime to explain.
    auto const mesh = build_cube_mesh();
    auto const indices = build_cube_indices();
    auto const vertices = ctx.transient.create_buffer<cube_vertex>(
        cube_vertex_count, sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    auto const index_buffer = ctx.transient.create_buffer<u16>(
        cube_index_count, sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst);
    auto const instances = ctx.transient.create_buffer<cube_instance>(
        _instances.size(), sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);

    cmd.upload.data_to_buffer(vertices, cc::span<cube_vertex const>(mesh));
    cmd.upload.data_to_buffer(index_buffer, cc::span<u16 const>(indices));
    cmd.upload.data_to_buffer(instances, cc::span<cube_instance const>(_instances));

    scope.bind_pipeline(*pipeline.value());
    scope.bind_vertex_buffers({vertices.as_vertex_buffer(), instances.as_vertex_buffer()});
    scope.bind_index_buffer(index_buffer.as_index_buffer());
    scope.set_inline_constants(view_projection);
    scope.draw_indexed({.index_range = {.offset = 0, .size = cube_index_count},
                        .instance_range = {.offset = 0, .size = _instances.size()}});
}
} // namespace cube_editor
