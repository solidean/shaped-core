#include "metal-test-common.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
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
// This one is the real path end to end — SGL emits MSL, ssc::msl compiles it, and the backend draws it — and the host
// side is the package's generated code too: the vertex struct and its layout, the constants block, the pipeline layout.
// So what it proves is that the addresses nobody wrote agree: `[[attribute(n)]]` against the generated layout, the
// inline-constants buffer index against metal_common.hh, and an interpolant against the stage that reads it.

namespace mtl = sg::backend::metal;
namespace shaders = metal_test::sgl_shaders;
using namespace cc::primitive_defines;

namespace
{
constexpr auto k_size = 8;

/// One triangle whose bounding square covers the whole target, so every texel it reaches is written by the shader.
[[nodiscard]] cc::vector<shaders::cube_vertex> covering_triangle()
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

/// What a settled `node` failed with, or empty where it succeeded.
[[nodiscard]] cc::string failure_of(sg::async_compiled_shader const& node)
{
    return node->has_error() ? node->try_error()->underlying().to_string() : cc::string();
}
} // namespace

ASYNC_TEST("sg metal - an SGL package compiles for metal", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));
    lib.add_package(shaders::package());

    auto const vs = shaders::cube.main_vs->acquire(*ctx);
    auto const ps = shaders::cube.main_ps->acquire(*ctx);
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    REQUIRE(vs->has_value()).context(failure_of(vs));
    REQUIRE(ps->has_value()).context(failure_of(ps));

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
    lib.add_package(shaders::package());

    auto const vs = shaders::cube.main_vs->acquire(*ctx);
    auto const ps = shaders::cube.main_ps->acquire(*ctx);
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);
    REQUIRE(vs->has_value()).context(failure_of(vs));
    REQUIRE(ps->has_value()).context(failure_of(ps));

    // Built through the cache, so the frontend's layout fit sees the reflection before the backend does.
    auto desc = sg::raster_pipeline_description{
        .layout = shaders::cube.main_vs.acquire_layout(*ctx),
        .vertex_shader = *vs->try_value(),
        .fragment_shader = *ps->try_value(),
        .vertex_input = shaders::cube_vertex::layout(),
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});

    auto const pipeline = ctx->cached.acquire_raster_pipeline(desc);
    co_await cc::async_settled(pipeline);
    REQUIRE(pipeline->has_value())
        .context(pipeline->has_error() ? pipeline->try_error()->underlying().to_string() : cc::string());

    auto const target = make_color_target(ctx);
    auto const vertices = covering_triangle();
    auto const vertex_buffer
        = ctx->persistent.create_raw_buffer(isize(vertices.size()) * isize(sizeof(shaders::cube_vertex)),
                                            sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);

    // One unit right in clip space, so the left half of the target keeps the clear colour.
    // Not the identity, which is its own transpose: a row/column-major mismatch would move the other half instead.
    auto const constants = shaders::constants{
        .view_projection = tg::rigid_transform3f::make_translation(tg::vec3f(1, 0, 0)).to_mat(),
    };

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(vertex_buffer, cc::as_bytes(cc::span<shaders::cube_vertex const>(vertices)));
    {
        auto info = sg::rendering_info{};
        // Red, so a texel the shader never wrote is loud in the readback.
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(**pipeline->try_value());
        scope.set_inline_constants(constants.to_block());
        scope.bind_vertex_buffer({.buffer = vertex_buffer, .stride_in_bytes = isize(sizeof(shaders::cube_vertex))});
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
    for (auto y = 0; y < k_size; ++y)
    {
        for (auto x = 0; x < k_size; ++x)
        {
            auto const* const texel = reinterpret_cast<byte const*>(bytes.value().data()) + (y * k_size + x) * 4;
            auto const is_shaded = x >= k_size / 2;
            auto const expected_r = is_shaded ? 64 : 255;
            auto const expected_g = is_shaded ? 128 : 0;
            auto const expected_b = is_shaded ? 191 : 0;
            if (u8(texel[0]) != expected_r || u8(texel[1]) != expected_g || u8(texel[2]) != expected_b)
                ++wrong;
        }
    }
    CHECK(wrong == 0)
        .context(cc::format("{} of {} texels are not what the translated draw leaves there", wrong, k_size * k_size));
}
