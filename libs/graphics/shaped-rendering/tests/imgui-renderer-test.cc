#include <clean-core/container/pinned_data.hh>
#include <imgui.h>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/imgui_renderer.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

// The imgui renderer end to end on a dx12 WARP device: build a frame of real imgui draw data, service the
// texture protocol, record the draws into an offscreen target, and read the pixels back.
//
// No window and no swapchain are involved — the target is a plain rgba8 texture with copy_src, which is
// how the repo's other raster tests run headless on CI.

namespace
{
constexpr auto target_width = 256;
constexpr auto target_height = 256;

/// Everything one frame needs, so each test reads as the frame it is testing rather than as setup.
/// Held by unique_ptr because the shader library is pinned to its address.
struct imgui_fixture
{
    sg::context_handle ctx;
    slib::shader_library shader_lib;
    sr::imgui_context imgui;
    sr::imgui_renderer renderer;
    sg::texture_2d target;

    /// Runs one full frame: build the UI, then prepare + render into `target`.
    template <class F>
    void frame(F&& build_ui)
    {
        imgui.begin_frame({.display_size = tg::vec2i(target_width, target_height), .delta_time = 1.0f / 60.0f});
        build_ui();
        imgui.end_frame();

        auto* const draw_data = ImGui::GetDrawData();

        auto cmd = ctx->create_command_list();
        renderer.prepare(*cmd, draw_data); // outside the scope: the geometry upload is a copy
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
            renderer.render(
                *cmd, draw_data,
                {.target_format = sg::pixel_format::rgba8_unorm, .target_size = tg::vec2i(target_width, target_height)});
        }
        ctx->submit_command_list(cc::move(cmd));

        // What a real frame ends with. Draining here also keeps each test self-contained: transient
        // geometry is recycled, and no GPU work is left in flight when the fixture is torn down.
        ctx->advance_epoch_and_wait_for_idle();
    }

    [[nodiscard]] cc::isize live_texture_count() const { return renderer.live_texture_count(); }

    [[nodiscard]] cc::pinned_data<cc::byte const> read_back()
    {
        auto cmd = ctx->create_command_list();
        auto const future = cmd->download.bytes_from_texture(target.raw());
        ctx->submit_command_list(cc::move(cmd));

        auto bytes = ctx->wait_for(future);
        REQUIRE(bytes.has_value());
        return cc::move(bytes).value();
    }
};

/// Builds a fixture, or null when this machine cannot run the test.
std::unique_ptr<imgui_fixture> make_fixture()
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (!ctx_r.has_value())
        return nullptr;
    if (!ctx_r.value()->accepts_shader_format(sg::shader_format::dxil))
        return nullptr;

    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        return nullptr;

    auto fixture = std::make_unique<imgui_fixture>();
    fixture->ctx = ctx_r.value();
    fixture->shader_lib.add_compiler(cc::move(compiler.value()));
    fixture->shader_lib.add_package(sr::shader_package());

    fixture->imgui = sr::imgui_context::create();
    fixture->renderer = sr::imgui_renderer::create(*fixture->ctx);
    fixture->target = fixture->ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm,
         .width = target_width,
         .height = target_height,
         .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    return fixture;
}

/// True if any pixel differs from the cleared background — i.e. imgui actually drew something.
[[nodiscard]] bool any_pixel_drawn(cc::span<cc::byte const> pixels)
{
    for (auto i = cc::isize(0); i + 3 < pixels.size(); i += 4)
        if (pixels[i] != cc::byte(0) || pixels[i + 1] != cc::byte(0) || pixels[i + 2] != cc::byte(0))
            return true;
    return false;
}

/// Red channel of the pixel at (x, y).
[[nodiscard]] cc::byte pixel_at(cc::span<cc::byte const> pixels, int x, int y)
{
    return pixels[(cc::isize(y) * target_width + x) * 4];
}

void draw_test_window()
{
    ImGui::SetNextWindowPos(ImVec2(16, 16));
    ImGui::SetNextWindowSize(ImVec2(200, 160));
    ImGui::Begin("test window");
    ImGui::Text("hello from shaped-rendering");
    ImGui::End();
}
} // namespace

TEST("sr::imgui_renderer - draws a window into an offscreen target")
{
    auto const f = make_fixture();
    if (f == nullptr)
        SKIP("no dx12 WARP device with DXIL + DXC");

    f->frame(&draw_test_window);
    auto const pixels = f->read_back();
    REQUIRE(pixels.size() == cc::isize(target_width) * cc::isize(target_height) * 4);

    // Deliberately not a golden-image comparison: that would break on every imgui version bump without
    // catching anything these checks do not.
    CHECK(any_pixel_drawn(pixels));

    // Inside the window imgui was told to draw, the clear color must be gone.
    CHECK(pixel_at(pixels, 100, 100) != cc::byte(0));

    // Far outside it, the clear color must survive — which is what proves the scissor is doing its job
    // rather than the whole target being painted.
    CHECK(pixel_at(pixels, 250, 250) == cc::byte(0));
}

TEST("sr::imgui_renderer - the font atlas is created once across frames")
{
    auto const f = make_fixture();
    if (f == nullptr)
        SKIP("no dx12 WARP device with DXIL + DXC");

    f->frame(&draw_test_window);
    CHECK(f->live_texture_count() == 1); // the font atlas

    // This is what actually proves the 1.92 texture protocol is implemented rather than accidentally
    // working on frame one: a backend that ignored the status would keep creating textures.
    f->frame(&draw_test_window);
    f->frame(&draw_test_window);
    CHECK(f->live_texture_count() == 1);
}

TEST("sr::imgui_renderer - a shader reload keeps drawing and keeps the atlas")
{
    auto const f = make_fixture();
    if (f == nullptr)
        SKIP("no dx12 WARP device with DXIL + DXC");

    f->frame(&draw_test_window);
    CHECK(f->live_texture_count() == 1);

    // A reload re-runs the routine's init_declare, which rebuilds the layouts. A pipeline still cached
    // against the old ones would now be stale — this is the check that the routine drops them.
    sg::signal_reload();

    f->frame(&draw_test_window);
    CHECK(any_pixel_drawn(f->read_back()));

    // The atlas lives on the renderer, not the routine, so a shader reload must not disturb it.
    CHECK(f->live_texture_count() == 1);
}

TEST("sr::imgui_renderer - an empty frame records nothing and does not assert")
{
    auto const f = make_fixture();
    if (f == nullptr)
        SKIP("no dx12 WARP device with DXIL + DXC");

    f->frame([] {}); // no windows at all

    auto const pixels = f->read_back();
    REQUIRE(pixels.size() == cc::isize(target_width) * cc::isize(target_height) * 4);
    CHECK(!any_pixel_drawn(pixels)); // still the clear color
}

TEST("sr::imgui_renderer - releasing textures lets a later frame rebuild them")
{
    auto const f = make_fixture();
    if (f == nullptr)
        SKIP("no dx12 WARP device with DXIL + DXC");

    f->frame(&draw_test_window);
    CHECK(f->live_texture_count() == 1);

    f->renderer.release_textures(ImGui::GetDrawData());
    CHECK(f->live_texture_count() == 0);

    // imgui still holds the pixels, so its side goes back to WantCreate and the next frame rebuilds.
    f->frame(&draw_test_window);
    CHECK(f->live_texture_count() == 1);
}
