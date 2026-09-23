#include "metal-test-common.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <metal_sgl_shaders.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/compiler/metal_compiler.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/all.hh>

// An SGL shader, drawn on metal.
//
// Every other fixture in this directory is MSL written by hand with its reflection written beside it, which pins the
// backend against a convention no real pipeline produces.
// This one is the real path end to end — SGL emits MSL, ssc::msl compiles it, and the backend draws it — so what it
// proves is that the addresses nobody wrote agree: `[[attribute(n)]]` against sg's vertex_input_layout, the
// inline-constants buffer index against metal_common.hh, and an interpolant against the stage that reads it.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
constexpr auto k_size = 8;

/// Mirrors `cube_vertex` in tests/shaders/cube.sgl, in the order its members are declared.
struct cube_vertex
{
    tg::pos3f position;
    tg::vec3f color;
};

/// The inline-constants block of the same shader.
struct cube_constants
{
    tg::mat4f view_projection = tg::mat4f::identity;
};

/// The vertex layout the shader's `@vertex struct` implies.
/// An attribute's `[[attribute(n)]]` index is its position here, which is the rule the SGL emitter writes against.
[[nodiscard]] sg::vertex_input_layout cube_vertex_layout()
{
    auto layout = sg::vertex_input_layout{};
    layout.slots.push_back({.stride = isize(sizeof(cube_vertex))});
    layout.attributes.push_back(
        {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec3f, .offset = 0, .slot = 0});
    layout.attributes.push_back(
        {.semantic = "COLOR", .format = sg::vertex_attribute_format::vec3f, .offset = isize(sizeof(tg::pos3f)), .slot = 0});
    return layout;
}

/// Two triangles covering the whole target, so every texel is written by the shader rather than by the clear.
[[nodiscard]] cc::vector<cube_vertex> full_target_quad()
{
    auto const color = tg::vec3f(0.25f, 0.5f, 0.75f);
    return {
        {.position = tg::pos3f(-1, -1, 0), .color = color},
        {.position = tg::pos3f(-1, 3, 0), .color = color},
        {.position = tg::pos3f(3, -1, 0), .color = color},
    };
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
} // namespace

TEST("sg metal - an SGL package compiles for metal", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));
    lib.add_package(metal_test::sgl_shaders::package());

    auto const vs = metal_test::sgl_shaders::cube.vertex.main_vs->acquire(*ctx);
    auto const ps = metal_test::sgl_shaders::cube.pixel.main_ps->acquire(*ctx);

    REQUIRE(vs->has_value()).context(vs->has_error() ? vs->try_error()->underlying().to_string() : cc::string());
    REQUIRE(ps->has_value()).context(ps->has_error() ? ps->try_error()->underlying().to_string() : cc::string());

    CHECK(vs->try_value()->stage == sg::shader_stage::vertex);
    CHECK(ps->try_value()->stage == sg::shader_stage::fragment);

    // Whichever arm ran, the context accepts the format — that is what `acquire(ctx)` resolves on.
    auto const format = vs->try_value()->format;
    auto const is_metal_format = format == sg::shader_format::metal_lib || format == sg::shader_format::msl;
    CHECK(is_metal_format);
}

ASYNC_TEST("sg metal - a draw from an SGL shader writes what the shader computed", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));
    lib.add_package(metal_test::sgl_shaders::package());

    auto const vs = metal_test::sgl_shaders::cube.vertex.main_vs->acquire(*ctx);
    auto const ps = metal_test::sgl_shaders::cube.pixel.main_ps->acquire(*ctx);
    REQUIRE(vs->has_value()).context(vs->has_error() ? vs->try_error()->underlying().to_string() : cc::string());
    REQUIRE(ps->has_value()).context(ps->has_error() ? ps->try_error()->underlying().to_string() : cc::string());

    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.inline_constants = sg::binding{
        .space = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::uniform_buffer,
        .block_size = isize(sizeof(cube_constants)),
    };
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto desc = sg::raster_pipeline_description{
        .layout = sg::pipeline_layout_handle(pipeline_layout.value()),
        .vertex_shader = *vs->try_value(),
        .fragment_shader = *ps->try_value(),
        .vertex_input = cube_vertex_layout(),
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});

    auto pipeline = ctx->create_metal_raster_pipeline(desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto const target = make_color_target(ctx);
    auto const vertices = full_target_quad();
    auto const vertex_buffer
        = ctx->persistent.create_raw_buffer(isize(vertices.size()) * isize(sizeof(cube_vertex)),
                                            sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(vertex_buffer, cc::as_bytes(cc::span<cube_vertex const>(vertices)));
    {
        auto info = sg::rendering_info{};
        // Red, so a texel the shader never wrote is loud in the readback.
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.set_inline_constants(cube_constants{});
        scope.bind_vertex_buffer({.buffer = vertex_buffer, .stride_in_bytes = isize(sizeof(cube_vertex))});
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto future = cmd->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == k_size * k_size * 4);

    // The vertex colour reaches the pixel stage through an interpolant the shader never addressed, so a wrong
    // `[[user(...)]]` mapping shows up here as the clear colour rather than as a build failure.
    auto wrong = 0;
    for (auto i = 0; i < k_size * k_size; ++i)
    {
        auto const* const texel = reinterpret_cast<byte const*>(bytes.value().data()) + i * 4;
        if (u8(texel[0]) != 64 || u8(texel[1]) != 128 || u8(texel[2]) != 191)
            ++wrong;
    }
    CHECK(wrong == 0).context(cc::format("{} of {} texels are not the shader's colour", wrong, k_size * k_size));
}
