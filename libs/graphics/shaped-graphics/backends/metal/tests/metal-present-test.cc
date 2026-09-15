#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/present/swapchain.hh>

// Presentation, headless.
//
// There is no windowed test here and there cannot be one at this tier: a CAMetalLayer needs a window, and this library
// sits below the one that has windows.
// The windowed path is exercised from shaped-viewer.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

TEST("sg metal - headless presentation is always supported")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Unconditionally true, where vulkan needs VK_EXT_headless_surface and reports what the instance actually has.
    // A headless chain here is ordinary render targets, so there is no extension to be missing.
    CHECK(ctx->supports(sg::feature::headless_present));
    CHECK(ctx->supports_headless_present());
}

ASYNC_TEST("sg metal - a headless chain hands out distinct back buffers")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const chain = ctx->create_swapchain({
        .headless_extent = tg::vec2i(8, 8),
        .buffer_count = 3,
        .format = sg::pixel_format::rgba8_unorm,
    });
    REQUIRE(chain != nullptr);

    CHECK(!chain->is_windowed());
    CHECK(chain->buffer_count() == 3);

    // A chain that handed out the same texture every frame would pass a "does it crash" test and fail every frame loop,
    // so the cycling is what is worth pinning.
    auto const first = chain->acquire_backbuffer();
    CHECK(first.width() == 8);
    CHECK(first.height() == 8);

    auto cmd = ctx->create_command_list();
    ctx->submit_command_list_and_present(*chain, cc::move(cmd));

    auto const second = chain->acquire_backbuffer();
    CHECK(second.texture() != first.texture());

    co_await ctx->idle_completion();
}

TEST("sg metal - a windowed chain refuses a non-cocoa window")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Metal presents into a CAMetalLayer, which sg spells window_platform::cocoa — so an HWND is not a thing this
    // backend can be handed, and saying so beats discovering it at the first present.
    auto const chain = ctx->create_metal_swapchain({.window = sg::native_window::from_win32(reinterpret_cast<void*>(1))});
    CHECK(chain.has_error());
}
