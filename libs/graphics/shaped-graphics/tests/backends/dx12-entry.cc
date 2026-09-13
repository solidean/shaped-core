#include "sg_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>           // sg::create_dx12_context
#include <shaped-graphics/backends/dx12/dx12_expected_messages.hh> // the allowlist both suites share

// dx12 entry-point drivers inside the sg API test binary (shaped-graphics-test).
// Each creates a dx12 context and invokes every sg::context_handle API test against it.
// Compiled only where the dx12 backend builds, so Windows.
// They carry the slib-shader-library tag because the invocables they dispatch stand up a slib::shader_library, which is a process-wide singleton.
// They carry sg-reload-generation because routine invocables count init runs, and a top-level test's sg::signal_reload would re-run them mid-test.
// A child's own exclusion tags schedule nothing, since it runs inside its driver's body, so the driver has to hold them.
// Two adapters are covered, both with the debug layer on:
//   - hardware: the real GPU; SKIPs when none is available (e.g. headless CI), and FAILs when one is and creation still fails.
//   - WARP (software): the sweep on a host with no GPU, and a second pass under --thorough on one that has it.

namespace
{
namespace dx12 = sg::backend::dx12;

// Fails whichever test provoked it on any debug-layer warning or worse, bar the advisories sg provokes on purpose.
// Without this a validation error is a line on stderr nobody reads, and the run stays green.
// The check lands on the right test wherever the runtime raised the message, since attribution rides the ambient context.
// A test that means to provoke one opts out by not installing this.
//
// The allowlist is dx12_expected_messages.hh rather than a copy here, because the tier-2 suite installs a listener of
// its own and the two disagreeing is a test that fails on a message the other binary already understood.
void fail_on_validation_messages(sg::context_handle const& ctx)
{
    static_cast<dx12::dx12_context&>(*ctx).set_message_callback(
        [](dx12::dx12_message_severity severity, cc::string_view message)
        {
            if (severity > dx12::dx12_message_severity::warning)
                return;
            if (dx12::is_expected_validation_message(message))
                return;

            CHECK(false).context(cc::format("dx12 debug layer: {}", message));
        });
}
} // namespace

TEST("sg dx12 warp backend", exclusive("slib-shader-library"), exclusive("sg-reload-generation"))
{
    // Beside a GPU, WARP is a second adapter the default run need not pay for; on a GPU-less host it is the only one.
    if (!nx::is_thorough() && sg::backend::dx12::has_hardware_adapter())
        SKIP("the hardware adapter covers the default run; WARP runs under --thorough");

    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::warp});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
    {
        fail_on_validation_messages(ctx.value());
        nx::invoke_tests("dx12-warp", ctx.value());
    }
}

TEST("sg dx12 hardware backend", exclusive("slib-shader-library"), exclusive("sg-reload-generation"))
{
    auto ctx
        = sg::create_dx12_context({.enable_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    // A host that has the adapter and still cannot bring up a device is broken, and a SKIP would hide it.
    if (ctx.has_error() && dx12::has_hardware_adapter())
        FAIL(cc::format("dx12 hardware device creation failed: {}", ctx.error().to_string()));
    else if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
    {
        fail_on_validation_messages(ctx.value());
        nx::invoke_tests("dx12-hw", ctx.value());
    }
}

static bool const sg_dx12_warp_registered = sg_test::register_backend("sg dx12 warp backend", "dx12-warp");
static bool const sg_dx12_hw_registered = sg_test::register_backend("sg dx12 hardware backend", "dx12-hw");
