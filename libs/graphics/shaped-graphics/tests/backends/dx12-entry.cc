#include "sg_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context

// dx12 entry-point drivers inside the sg API test binary (shaped-graphics-test).
// Each creates a dx12 context and invokes every sg::context_handle API test against it.
// Compiled only where the dx12 backend builds, so Windows.
// They carry the slib-shader-library tag because the invocables they dispatch stand up a slib::shader_library, which is a process-wide singleton.
// Two adapters are covered, both with the debug layer on:
//   - WARP (software): present on any Windows host, so it also runs headless on CI.
//   - hardware: the real GPU; SKIPs when none is available (e.g. headless CI).

namespace
{
namespace dx12 = sg::backend::dx12;

// Fails whichever test provoked it on any debug-layer warning or worse.
// Without this a validation error is a line on stderr nobody reads, and the run stays green.
// The check lands on the right test wherever the runtime raised the message, since attribution rides the ambient context.
// A test that means to provoke one opts out by not installing this.
void fail_on_validation_messages(sg::context_handle const& ctx)
{
    static_cast<dx12::dx12_context&>(*ctx).set_message_callback(
        [](dx12::dx12_message_severity severity, cc::string_view message)
        {
            if (severity <= dx12::dx12_message_severity::warning)
                CHECK(false).context(cc::format("dx12 debug layer: {}", message));
        });
}
} // namespace

TEST("sg dx12 warp backend", exclusive("slib-shader-library"))
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
    {
        fail_on_validation_messages(ctx.value());
        nx::invoke_tests("dx12-warp", ctx.value());
    }
}

TEST("sg dx12 hardware backend", exclusive("slib-shader-library"))
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = false});
    if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
    {
        fail_on_validation_messages(ctx.value());
        nx::invoke_tests("dx12-hw", ctx.value());
    }
}

static bool const sg_dx12_warp_registered = sg_test::register_backend("sg dx12 warp backend", "dx12-warp");
static bool const sg_dx12_hw_registered = sg_test::register_backend("sg dx12 hardware backend", "dx12-hw");
