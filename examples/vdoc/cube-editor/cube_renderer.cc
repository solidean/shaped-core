#include "cube_renderer.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
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

        // Reversed relative to the corner order below, because the (u, v, n) basis those corners are laid out in is
        // right-handed while the camera's projection is left-handed: a face wound counter-clockwise in that basis
        // reaches the screen clockwise. sg's default front face is counter-clockwise, so the un-reversed order
        // culls every outward face and leaves the cube inside out.
        u16 const quad[] = {0, 2, 1, 0, 3, 2};
        for (auto i = 0; i < 6; ++i)
            out[face * 6 + i] = u16(base + quad[i]);
    }
    return out;
}
} // namespace

// Neither stream states its layout here any more: cube.hlsl declares both structs and the package generates
// `sg::vertex_layout_of` for each, so the semantics, the formats, the offsets and the two slots are the shader's.
// These two types stay because filling them is more readable in tg's, and the asserts are what keeps them the
// same bytes rather than a comment asking someone to.
static_assert(sizeof(cube_vertex) == sizeof(cube_editor::shaders::vs_input),
              "cube_vertex is not the stride cube.hlsl states");
static_assert(offsetof(cube_vertex, position) == offsetof(cube_editor::shaders::vs_input, position), "position moved");
static_assert(offsetof(cube_vertex, normal) == offsetof(cube_editor::shaders::vs_input, normal), "normal moved");

static_assert(sizeof(cube_editor::cube_instance) == sizeof(cube_editor::shaders::instance_input),
              "cube_instance is not the stride cube.hlsl states");
static_assert(offsetof(cube_editor::cube_instance, center) == offsetof(cube_editor::shaders::instance_input, center),
              "center moved");
static_assert(offsetof(cube_editor::cube_instance, half_extent)
                  == offsetof(cube_editor::shaders::instance_input, half_extent),
              "half_extent moved");
static_assert(offsetof(cube_editor::cube_instance, color) == offsetof(cube_editor::shaders::instance_input, color),
              "color moved");
static_assert(offsetof(cube_editor::cube_instance, highlight)
                  == offsetof(cube_editor::shaders::instance_input, highlight),
              "highlight moved");

// And the constant block, whose one float4x4 the pass mirrors with HLSL's own packing.
static_assert(sizeof(tg::mat4f) == sizeof(cube_editor::shaders::cube_constants),
              "the view-projection matrix is not the size cube.hlsl's block states");

namespace
{
/// Why one shader acquire produced no value, in words worth printing.
[[nodiscard]] cc::string acquire_failure(sg::async_compiled_shader const& shader)
{
    auto const* const error = shader->try_error();
    if (error == nullptr)
        return cc::string("no result and no error — the compile never ran");
    if (error->is_cancelled())
        return cc::string("the compile was cancelled");

    return error->underlying().to_string();
}
} // namespace

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

cc::shared_async<cc::unit> cube_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    auto vs = shaders::cube.vertex.main_vs->acquire(ctx);
    auto ps = shaders::cube.fragment.main_ps->acquire(ctx);

    // Settled rather than awaited for the value: a shader that did not compile is this routine's verdict to report,
    // not an error to propagate.
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    _pipeline = {};
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        // The compiler's diagnostics ride on the async's failure channel, so reporting only "it did not compile"
        // throws away the one thing worth reading.
        auto message = cc::string("cube.hlsl did not compile");
        if (compiled_vs == nullptr)
            message += cc::format("\n  vertex main_vs: {}", acquire_failure(vs));
        if (compiled_ps == nullptr)
            message += cc::format("\n  fragment main_ps: {}", acquire_failure(ps));
        CC_LOG_ERROR("{}", message);
        fail_init(); // not pending: this will not come good until a reload, and a caller should be able to tell
        co_return;
    }

    // The only binding is the vertex stage's 64-byte view-projection block, and it rides as inline constants --
    // so there are no binding groups at all, which is why nothing here builds one.
    auto const* const constants = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled_vs->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    if (constants == nullptr)
    {
        CC_LOG_ERROR("cube.hlsl must declare the cube_constants cbuffer");
        fail_init();
        co_return;
    }

    auto const layout = ctx.cached.acquire_pipeline_layout({.inline_constants = *constants});

    // One instance means one pipeline to build, rather than a map filled lazily on the frame path.
    //
    // The input layout is stated in the generated structs rather than in the two local ones, so the shader is
    // what defines the stride and the static_asserts above are what hold the CPU side to it.
    _pipeline = ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = layout,
        .vertex_shader = *compiled_vs,
        .fragment_shader = *compiled_ps,
        .vertex_input = sg::vertex_input_layout::create<shaders::vs_input, shaders::instance_input>(),
        .rasterization = {.cull = sg::cull_mode::back},
        // Both default to OFF, and solid geometry needs both -- a cube drawn without them shows whichever face
        // happened to be recorded last.
        .depth_stencil = {.depth_test = true, .depth_write = true},
        .color_targets = {{.format = params()}},
        .depth_stencil_format = sg::pixel_format::depth32_float});

    // Awaited HERE rather than polled in execute, so `ready` means ready.
    co_await cc::async_settled(_pipeline);
    co_return;
}

sg::routine_outcome cube_routine::execute(sg::rendering_scope& scope,
                                          vdoc::document const& doc,
                                          tg::mat4f const& view_projection,
                                          vdoc::entity_id selected)
{
    auto& cmd = scope.command_list();
    auto& ctx = cmd.context();
    CC_ASSERT(!scope.color_formats().empty(), "the cube pass must be drawn into a scope with a color target");

    // The format picks the instance, and it is only knowable here -- which is what makes this routine fallible.
    // Exclusive because the instance buffer below is the routine's own state.
    auto self = try_acquire_exclusive(cmd, scope.color_formats()[0]);
    if (!self.is_ready())
        return sg::routine_outcome::declined;

    // Polled rather than waited on: execute runs inside the caller's rendering scope, so a wait would stall a frame
    // that has a pass open, and a throw would leave their command list unsubmitted.
    auto const* const pipeline = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (pipeline == nullptr || *pipeline == nullptr)
        return sg::routine_outcome::declined;

    self->_instances = collect_instances(doc, selected);
    if (self->_instances.empty())
        return sg::routine_outcome::executed; // an empty document has nothing to draw and nothing to report

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
        self->_instances.size(), sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);

    cmd.upload.data_to_buffer(vertices, cc::span<cube_vertex const>(mesh));
    cmd.upload.data_to_buffer(index_buffer, cc::span<u16 const>(indices));
    cmd.upload.data_to_buffer(instances, cc::span<cube_instance const>(self->_instances));

    scope.bind_pipeline(**pipeline);
    scope.bind_vertex_buffers({vertices.as_vertex_buffer(), instances.as_vertex_buffer()});
    scope.bind_index_buffer(index_buffer.as_index_buffer());
    scope.set_inline_constants(view_projection);
    scope.draw_indexed({.index_range = {.offset = 0, .size = cube_index_count},
                        .instance_range = {.offset = 0, .size = self->_instances.size()}});
    return sg::routine_outcome::executed;
}
} // namespace cube_editor
