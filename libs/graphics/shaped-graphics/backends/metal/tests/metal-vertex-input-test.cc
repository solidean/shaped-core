#include "mesh.metallib.h"
#include "metal-test-common.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

// Vertex input, indexed draws and inline constants, end to end.
//
// MTL4's render encoder has no `setVertexBuffer` and Metal has no root constants, so all three reach a draw as
// addresses in the one argument table — which means nothing about them is checked by the API, and a wrong buffer index
// draws something plausible instead of failing.
// These tests are what says the two halves agree: the vertex descriptor the pipeline was built with, and the slots the
// command list writes.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
constexpr auto k_size = 4;

/// One vertex of the fixture's quad: a clip-space position, then a colour — the layout `mesh.metal`'s `vertex_in`
/// declares, with the colour at offset 8.
struct mesh_vertex
{
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

/// The inline-constants block `mesh.metal` reads at [[buffer(4)]].
struct tint_constants
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

[[nodiscard]] sg::compiled_shader mesh_stage(sg::shader_stage stage, cc::string entry)
{
    auto shader = sg::compiled_shader{};
    shader.stage = stage;
    shader.format = sg::shader_format::metal_lib;
    shader.entry_point = cc::move(entry);

    auto blob = cc::pinned_data<byte>::create_uninitialized(isize(sizeof(mtl::test::mesh_metallib)));
    cc::memcpy(blob.data(), mtl::test::mesh_metallib, sizeof(mtl::test::mesh_metallib));
    shader.bytecode = cc::pinned_data<byte const>(cc::move(blob));
    return shader;
}

/// The vertex-input layout of `mesh_vertex`, written by hand beside the shader it has to agree with.
///
/// An attribute's `[[attribute(n)]]` index is its position in this list — the same rule the vulkan backend states for
/// its SPIR-V locations, and the same workaround for sg identifying an input by an HLSL semantic string.
[[nodiscard]] sg::vertex_input_layout mesh_vertex_layout()
{
    auto layout = sg::vertex_input_layout{};
    layout.slots.push_back({.stride = isize(sizeof(mesh_vertex))});
    layout.attributes.push_back(
        {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec2f, .offset = 0, .slot = 0});
    layout.attributes.push_back(
        {.semantic = "COLOR", .format = sg::vertex_attribute_format::vec4f, .offset = 8, .slot = 0});
    return layout;
}

/// The pipeline layout the fixture's draw uses: no binding groups, one 16-byte inline-constants block.
[[nodiscard]] cc::result<sg::pipeline_layout_handle> make_tint_pipeline_layout(mtl::metal_context_handle const& ctx)
{
    auto desc = sg::pipeline_layout_description{};
    desc.inline_constants = sg::binding{
        .space = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::uniform_buffer,
        .block_size = isize(sizeof(tint_constants)),
    };

    auto layout = ctx->create_metal_pipeline_layout(desc, sg::lifetime_scope::persistent);
    if (layout.has_error())
        return cc::error(layout.error().to_string());
    return sg::pipeline_layout_handle(layout.value());
}

[[nodiscard]] cc::result<mtl::metal_raster_pipeline_handle> make_mesh_pipeline(mtl::metal_context_handle const& ctx,
                                                                               sg::pipeline_layout_handle layout)
{
    auto desc = sg::raster_pipeline_description{
        .layout = cc::move(layout),
        .vertex_shader = mesh_stage(sg::shader_stage::vertex, "vertex_main"),
        .fragment_shader = mesh_stage(sg::shader_stage::fragment, "fragment_main"),
        .vertex_input = mesh_vertex_layout(),
        // No culling: what these tests assert is which vertices were fetched, not which way the quad happens to wind.
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});

    return ctx->create_metal_raster_pipeline(desc, sg::lifetime_scope::persistent);
}

/// The fixture's vertex buffer contents.
///
/// **The first two vertices are decoys at the same point**, so a triangle that reaches them has zero area and writes
/// nothing — which is what makes a draw that ignored `vertex_offset` read back the clear instead of the quad.
[[nodiscard]] cc::vector<mesh_vertex> quad_vertices()
{
    auto out = cc::vector<mesh_vertex>();
    out.push_back({.x = 0.0f, .y = 0.0f});
    out.push_back({.x = 0.0f, .y = 0.0f});
    out.push_back({.x = -1.0f, .y = -1.0f, .r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f});
    out.push_back({.x = 1.0f, .y = -1.0f, .r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f});
    out.push_back({.x = -1.0f, .y = 1.0f, .r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f});
    out.push_back({.x = 1.0f, .y = 1.0f, .r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f});
    return out;
}

/// The fixture's indices, decoys first for the same reason the vertices have them.
/// A draw that ignored the first-index offset starts among the decoys and covers a different part of the target.
///
/// **Four decoys rather than three**, because Metal reads indices from a 4-byte boundary and the first index is folded
/// into the address — an odd first index into a 16-bit buffer is a draw the backend refuses, not one this fixture
/// means to exercise.
[[nodiscard]] cc::vector<u16> quad_indices()
{
    auto out = cc::vector<u16>();
    out.push_back(0);
    out.push_back(1);
    out.push_back(0);
    out.push_back(1);

    out.push_back(0);
    out.push_back(1);
    out.push_back(2);
    out.push_back(2);
    out.push_back(1);
    out.push_back(3);
    return out;
}

[[nodiscard]] auto make_color_target(mtl::metal_context_handle const& ctx)
{
    return ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
    });
}

/// 8-bit unorm comparison with a texel of slack for the rounding Metal chooses.
[[nodiscard]] bool near(byte value, int expected)
{
    auto const delta = int(u8(value)) - expected;
    return (delta < 0 ? -delta : delta) <= 1;
}

/// How many of a 4×4 rgba8 readback's texels are not `expected`.
[[nodiscard]] int texels_not(cc::span<byte const> bytes, int r, int g, int b)
{
    auto wrong = 0;
    for (auto i = 0; i < k_size * k_size; ++i)
    {
        auto const* const texel = &bytes[i * 4];
        if (!near(texel[0], r) || !near(texel[1], g) || !near(texel[2], b))
            ++wrong;
    }
    return wrong;
}
} // namespace

ASYNC_TEST("sg metal - an indexed draw reads its vertices, its indices and its base vertex")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto layout = make_tint_pipeline_layout(ctx);
    REQUIRE(layout.has_value()).context(layout.has_error() ? layout.error().to_string() : cc::string());

    auto pipeline = make_mesh_pipeline(ctx, layout.value());
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto const target = make_color_target(ctx);

    auto const vertices = quad_vertices();
    auto const indices = quad_indices();

    auto const vertex_buffer
        = ctx->persistent.create_raw_buffer(isize(vertices.size()) * isize(sizeof(mesh_vertex)),
                                            sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    auto const index_buffer = ctx->persistent.create_raw_buffer(
        isize(indices.size()) * isize(sizeof(u16)), sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(vertex_buffer, cc::as_bytes(cc::span<mesh_vertex const>(vertices)));
    cmd->upload.bytes_to_buffer(index_buffer, cc::as_bytes(cc::span<u16 const>(indices)));
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.set_inline_constants(tint_constants{});

        scope.bind_vertex_buffer({.buffer = vertex_buffer, .stride_in_bytes = isize(sizeof(mesh_vertex))});
        scope.bind_index_buffer({.buffer = index_buffer, .format = sg::index_format::uint16});

        // Past the decoys on both sides: four indices in, and two vertices up.
        scope.draw_indexed({.index_range = {.offset = 4, .size = 6}, .vertex_offset = 2});
    }
    auto future = cmd->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == k_size * k_size * 4);

    // The quad covers the whole target with the vertex colour, untinted.
    // The quad covers the whole target with the vertex colour, untinted.
    auto const wrong = texels_not(bytes.value(), 64, 128, 191);
    CHECK(wrong == 0)
        .context(cc::format("{} of {} texels wrong; first is {} {} {}", wrong, k_size * k_size,
                            int(u8(bytes.value()[0])), int(u8(bytes.value()[1])), int(u8(bytes.value()[2]))));
}

ASYNC_TEST("sg metal - raster inline constants reach the shader, and a changed block restages")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto layout = make_tint_pipeline_layout(ctx);
    REQUIRE(layout.has_value());
    auto pipeline = make_mesh_pipeline(ctx, layout.value());
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    // **Two passes in one list, differing only in their constants.**
    // The block is staged lazily and an unchanged one is bound again at the address it already has, so a backend that
    // forgot to restage a *changed* block would paint both targets the first pass's colour.
    auto const untinted = make_color_target(ctx);
    auto const halved = make_color_target(ctx);

    auto const vertices = quad_vertices();
    auto const indices = quad_indices();
    auto const vertex_buffer
        = ctx->persistent.create_raw_buffer(isize(vertices.size()) * isize(sizeof(mesh_vertex)),
                                            sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    auto const index_buffer = ctx->persistent.create_raw_buffer(
        isize(indices.size()) * isize(sizeof(u16)), sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(vertex_buffer, cc::as_bytes(cc::span<mesh_vertex const>(vertices)));
    cmd->upload.bytes_to_buffer(index_buffer, cc::as_bytes(cc::span<u16 const>(indices)));

    auto const pass = [&](auto const& target, tint_constants tint)
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.set_inline_constants(tint);

        scope.bind_vertex_buffer({.buffer = vertex_buffer, .stride_in_bytes = isize(sizeof(mesh_vertex))});
        scope.bind_index_buffer({.buffer = index_buffer, .format = sg::index_format::uint16});
        scope.draw_indexed({.index_range = {.offset = 4, .size = 6}, .vertex_offset = 2});
    };

    pass(untinted, tint_constants{});
    pass(halved, tint_constants{.r = 0.5f, .g = 0.5f, .b = 0.5f});

    auto untinted_future = cmd->download.bytes_from_texture(untinted.raw());
    auto halved_future = cmd->download.bytes_from_texture(halved.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const untinted_bytes = untinted_future.try_get_bytes();
    auto const halved_bytes = halved_future.try_get_bytes();
    REQUIRE(untinted_bytes.has_value());
    REQUIRE(halved_bytes.has_value());

    CHECK(texels_not(untinted_bytes.value(), 64, 128, 191) == 0)
        .context(cc::format("the untinted pass wrote {} {} {}", int(u8(untinted_bytes.value()[0])),
                            int(u8(untinted_bytes.value()[1])), int(u8(untinted_bytes.value()[2]))));
    CHECK(texels_not(halved_bytes.value(), 32, 64, 96) == 0)
        .context(cc::format("the halved pass wrote {} {} {}", int(u8(halved_bytes.value()[0])),
                            int(u8(halved_bytes.value()[1])), int(u8(halved_bytes.value()[2]))));
}

ASYNC_TEST("sg metal - a draw reads the vertex buffer a dispatch in the same list wrote")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // **The dependency crosses an encoder boundary**, which an encoder-scoped barrier cannot express: the dispatch
    // records into the compute encoder and the draw into a render encoder opened after it.
    // What is meant to order them is the publish each encoder ends with and the queue wait the next one opens with.
    //
    // **It covers the path rather than the barrier**, and the difference is worth stating: removing the publish leaves
    // this passing on an M4, because a four-thread dispatch finishes well before the pass it precedes.
    // So read a failure here as the dispatch-to-draw path being broken, not as the ordering being unprotected — what
    // would catch that needs a dispatch long enough to lose the race, which is a slow test rather than a sharp one.
    // libs/graphics/shaped-graphics/docs/TODO.md carries it.
    auto emit = mesh_stage(sg::shader_stage::compute, "emit_quad_main");
    emit.workgroup_size = sg::compute_dimensions{.x = 1, .y = 1, .z = 1};
    emit.bindings.push_back({
        .name = "vertices",
        .space = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });

    auto group_layout = ctx->create_metal_binding_group_layout(emit.bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(group_layout.has_value());

    auto emit_layout_desc = sg::pipeline_layout_description{};
    emit_layout_desc.groups.push_back(group_layout.value());
    auto emit_layout = ctx->create_metal_pipeline_layout(emit_layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(emit_layout.has_value());

    auto emit_pipeline = ctx->create_metal_compute_pipeline({.shader = emit, .layout = emit_layout.value()},
                                                            sg::lifetime_scope::persistent);
    REQUIRE(emit_pipeline.has_value()).context(emit_pipeline.has_error() ? emit_pipeline.error().to_string() : cc::string());

    auto draw_layout = make_tint_pipeline_layout(ctx);
    REQUIRE(draw_layout.has_value());
    auto pipeline = make_mesh_pipeline(ctx, draw_layout.value());
    REQUIRE(pipeline.has_value());

    auto const target = make_color_target(ctx);

    // Four vertices, written by the kernel and then fetched by the draw — so this buffer is both a shader write and a
    // vertex read, which is the hazard under test.
    auto const vertex_buffer = ctx->persistent.create_raw_buffer(
        4 * isize(sizeof(mesh_vertex)), sg::buffer_usage::vertex_buffer | sg::buffer_usage::readwrite_buffer);

    auto const nv = sg::named_view{.name = "vertices",
                                   .view = vertex_buffer->as_raw_readwrite(
                                       {.offset = 0, .size = vertex_buffer->size_in_bytes()}, isize(sizeof(float)))};
    auto group = ctx->create_metal_binding_group(group_layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value()).context(group.has_error() ? group.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*emit_pipeline.value());
    cmd->compute.bind_group(0, *group.value());
    cmd->compute.dispatch_groups(4, 1, 1);
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.set_inline_constants(tint_constants{});

        scope.bind_vertex_buffer({.buffer = vertex_buffer, .stride_in_bytes = isize(sizeof(mesh_vertex))});

        // Two triangles over the four written vertices, drawn without an index buffer.
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
        scope.draw({.vertex_range = {.offset = 1, .size = 3}});
    }
    auto future = cmd->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto const wrong = texels_not(bytes.value(), 64, 128, 191);
    CHECK(wrong == 0)
        .context(cc::format("{} of {} texels wrong; first is {} {} {}", wrong, k_size * k_size,
                            int(u8(bytes.value()[0])), int(u8(bytes.value()[1])), int(u8(bytes.value()[2]))));
}
