#include <clean-core/string/print.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/metal/metal_context.hh> // sg::create_metal_context
#include <shaped-rendering/window.hh>

using namespace cc::primitive_defines;

// The one test that crosses sr's window abstraction into metal's windowed presentation.
//
// Every other metal test presents headlessly, so nothing else exercises the cocoa path at all:
// SDL_WINDOW_METAL at window creation, the CAMetalLayer behind SDL_Metal_GetLayer, and the swapchain metal builds on it.
//
//     uv run dev.py test "sr - metal presents to a window (manual)" --manual
//
// Manual because it needs a display, like every other test in window-manual-test.cc.

namespace
{
// Two full cycles of a three-deep chain, so a back buffer is reused rather than merely handed out once.
constexpr int k_frames = 7;
} // namespace

ASYNC_TEST("sr - metal presents to a window (manual)", nx::config::manual, main_thread, exclusive("sr-window-system"))
{
    auto const wsys = sr::window_system::create();
    auto const win
        = wsys->create_window({.title = "metal present probe", .width = 320, .height = 240, .is_visible = false});

    auto const native = win->native_window();
    REQUIRE(native.is_valid());
    REQUIRE(native.platform == sg::window_platform::cocoa);

    auto const ctx_r = sg::create_metal_context({});
    REQUIRE(ctx_r.has_value());
    auto const ctx = ctx_r.value();

    auto const swapchain = ctx->create_swapchain({.window = native, .format = sg::pixel_format::bgra8_unorm});
    REQUIRE(swapchain != nullptr);
    CHECK(swapchain->is_windowed());

    for (int frame = 0; frame < k_frames; ++frame)
    {
        auto const rt = swapchain->acquire_backbuffer();
        CHECK(rt.width() > 0);
        CHECK(rt.height() > 0);

        auto cmd = ctx->create_command_list();
        {
            auto const shade = float(frame) / float(k_frames - 1);
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0, shade, 1 - shade, 1))}});
        }
        ctx->submit_command_list_and_present(*swapchain, cc::move(cmd));

        co_await ctx->idle_completion();
        ctx->advance_epoch();
        ctx->block_until_epochs_in_flight(2);
    }

    CHECK(!ctx->is_device_lost());
}
