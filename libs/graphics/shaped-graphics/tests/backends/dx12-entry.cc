#include "sg_backends.hh"

#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context

// dx12 entry-point drivers inside the sg API test binary (shaped-graphics-test). Each creates a dx12 context
// and invokes every sg::context_handle API test against it. Compiled only where the dx12 backend builds
// (Windows). Two adapters are covered, both with the debug layer on:
//   - WARP (software): present on any Windows host, so it also runs headless on CI.
//   - hardware: the real GPU; SKIPs when none is available (e.g. headless CI).

TEST("sg dx12 warp backend")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
        nx::invoke_tests("dx12-warp", ctx.value());
}

TEST("sg dx12 hardware backend")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = false});
    if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
        nx::invoke_tests("dx12-hw", ctx.value());
}

static bool const sg_dx12_warp_registered = sg_test::register_backend("sg dx12 warp backend", "dx12-warp");
static bool const sg_dx12_hw_registered = sg_test::register_backend("sg dx12 hardware backend", "dx12-hw");
