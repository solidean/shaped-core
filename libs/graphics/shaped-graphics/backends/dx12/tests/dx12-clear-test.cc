#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// End-to-end rendering-scope clears, driven through the backend-agnostic sg:: API (the WARP context is
// only the vehicle): open a raster rendering scope over a render-target / depth-stencil texture, clear it,
// read it back, and verify the clear value landed in every texel. Also covers the explicit
// cmd.raster.manual path, a discard (contents undefined -> smoke only), and an explicit viewport /
// scissor. There is no graphics pipeline yet, so a scope only applies its begin-ops.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::texture_description target_desc(sg::texture_usage usage, sg::pixel_format format, int w, int h)
{
    sg::texture_description d;
    d.format = format;
    d.dimension = sg::texture_dimension::d2;
    d.width = w;
    d.height = h;
    d.usage = usage;
    return d;
}
} // namespace

TEST("sg dx12 - clear render target fills every texel (render_to)")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    constexpr int W = 16;
    constexpr int H = 16;
    constexpr int N = W * H;
    auto tex = ctx->persistent.create_raw_texture(target_desc(
        sg::texture_usage::render_target | sg::texture_usage::copy_src, sg::pixel_format::rgba8_unorm, W, H));
    REQUIRE(tex != nullptr);
    auto const typed = sg::texture_2d::from_raw(tex);

    auto cmd = ctx->create_command_list();
    {
        // view.cleared(color) => clear; the RAII scope calls end_rendering when it goes out of scope.
        auto pass
            = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1))}});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(N) * 4);
    auto const* px = reinterpret_cast<cc::u8 const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (px[i * 4 + 0] != 255 || px[i * 4 + 1] != 0 || px[i * 4 + 2] != 0 || px[i * 4 + 3] != 255)
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - clear depth target fills every texel")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    constexpr int W = 16;
    constexpr int H = 16;
    constexpr int N = W * H;
    auto tex = ctx->persistent.create_raw_texture(target_desc(
        sg::texture_usage::depth_stencil | sg::texture_usage::copy_src, sg::pixel_format::depth32_float, W, H));
    REQUIRE(tex != nullptr);
    auto const typed = sg::texture_2d::from_raw(tex);

    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to({.depth_stencil_target = typed.as_depth_stencil_view().cleared(0.5f)});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(N) * cc::isize(sizeof(float)));
    auto const* depth = reinterpret_cast<float const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (depth[i] != 0.5f)
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - clear render target via the explicit manual scope")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    constexpr int W = 8;
    constexpr int H = 8;
    constexpr int N = W * H;
    auto tex = ctx->persistent.create_raw_texture(target_desc(
        sg::texture_usage::render_target | sg::texture_usage::copy_src, sg::pixel_format::rgba8_unorm, W, H));
    REQUIRE(tex != nullptr);
    auto const typed = sg::texture_2d::from_raw(tex);

    sg::rendering_info info;
    info.color_targets.push_back(typed.as_render_target_view().cleared(tg::vec4f(0, 1, 0, 1)));

    auto cmd = ctx->create_command_list();
    cmd->raster.manual.begin_rendering(info);
    cmd->raster.manual.end_rendering();
    auto future = cmd->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    auto const* px = reinterpret_cast<cc::u8 const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (px[i * 4 + 0] != 0 || px[i * 4 + 1] != 255 || px[i * 4 + 2] != 0 || px[i * 4 + 3] != 255)
            ok = false;
    CHECK(ok);
}

TEST("sg dx12 - discard render target records and executes")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    constexpr int W = 8;
    constexpr int H = 8;
    auto tex = ctx->persistent.create_raw_texture(target_desc(
        sg::texture_usage::render_target | sg::texture_usage::copy_src, sg::pixel_format::rgba8_unorm, W, H));
    REQUIRE(tex != nullptr);
    auto const typed = sg::texture_2d::from_raw(tex);

    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().discarded()}});
    }
    // Contents are undefined after a discard, so only assert the path records + runs (WARP's debug layer is
    // the oracle: a bad command would fault before the readback resolves).
    auto future = cmd->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = ctx->wait_for(future);
    CHECK(bytes.has_value());
}

TEST("sg dx12 - clear with an explicit viewport and scissor")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    constexpr int W = 16;
    constexpr int H = 16;
    constexpr int N = W * H;
    auto tex = ctx->persistent.create_raw_texture(target_desc(
        sg::texture_usage::render_target | sg::texture_usage::copy_src, sg::pixel_format::rgba8_unorm, W, H));
    REQUIRE(tex != nullptr);
    auto const typed = sg::texture_2d::from_raw(tex);

    // ClearRenderTargetView ignores the viewport/scissor (it clears the whole RTV), so the whole texture is
    // still filled — this exercises the explicit-viewport / explicit-scissor code paths, not per-pixel masking.
    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to({
            .color_targets = {typed.as_render_target_view().cleared(tg::vec4f(0, 0, 1, 1))},
            .viewport = sg::viewport{.offset = tg::pos2f(0, 0), .size = tg::vec2f(float(W), float(H))},
            .scissor = tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(W, H)),
        });
    }
    auto future = cmd->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    auto const* px = reinterpret_cast<cc::u8 const*>(bytes.value().data());
    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (px[i * 4 + 0] != 0 || px[i * 4 + 1] != 0 || px[i * 4 + 2] != 255 || px[i * 4 + 3] != 255)
            ok = false;
    CHECK(ok);
}
