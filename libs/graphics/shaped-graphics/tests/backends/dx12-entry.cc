#include "sg_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>           // sg::create_dx12_context
#include <shaped-graphics/backends/dx12/dx12_expected_messages.hh> // the advisories sg provokes on purpose

// dx12 entry-point drivers inside the sg API test binary (shaped-graphics-test).
// Each creates a dx12 context and invokes every sg::context_handle API test against it.
// Compiled only where the dx12 backend builds, so Windows.
// They hold no exclusion tags: an async invocation takes each child's own tags around its run, and a driver holding
// them too would be refused — so the invocables that stand up a slib::shader_library or count routine init runs say so.
// Two adapters are covered, both with the debug layer on:
//   - hardware: the real GPU; SKIPs when none is available (e.g. headless CI), and FAILs when one is and creation still fails.
//   - WARP (software): the sweep on a host with no GPU, and a second pass under --thorough on one that has it.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

// The debug-layer advisories sg provokes on purpose (dx12_expected_messages.hh), allowed in every test of this binary.
// Validation fails a test through the log rule rather than through a listener, so anything else the layer says still fails it.
NX_ALLOW_LOGS(cc::rec::level::warning, "sg.dx12", sg::backend::dx12::k_expected_validation_messages);


ASYNC_TEST("sg dx12 warp backend")
{
    // Beside a GPU, WARP is a second adapter the default run need not pay for; on a GPU-less host it is the only one.
    if (!nx::is_thorough() && sg::backend::dx12::has_hardware_adapter())
        SKIP("the hardware adapter covers the default run; WARP runs under --thorough");

    auto ctx = sg::create_dx12_context(
        {.activate_global_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::warp});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
    {
        co_await nx::async_invoke_tests_in_sequence("dx12-warp", ctx.value());

        // A device reset during our own tests is a defect, not an environment quirk to tolerate.
        // Checking once here rather than per-test is what makes it unmissable: the loss flag is sticky, so the
        // run fails whichever invocable lost the device.
        // The poll is what makes it reliable -- a reset nothing has submitted against yet is invisible to the flag.
        auto& dx = static_cast<dx12::dx12_context&>(*ctx.value());
        dx.poll_device_removal();
        CHECK(!dx.is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}", dx.device_loss_reason()));
    }
}

ASYNC_TEST("sg dx12 hardware backend")
{
    auto ctx = sg::create_dx12_context(
        {.activate_global_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    // A host that has the adapter and still cannot bring up a device is broken, and a SKIP would hide it.
    if (ctx.has_error() && dx12::has_hardware_adapter())
        FAIL(cc::format("dx12 hardware device creation failed: {}", ctx.error().to_string()));
    else if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
    {
        co_await nx::async_invoke_tests_in_sequence("dx12-hw", ctx.value());

        // A device reset during our own tests is a defect, not an environment quirk to tolerate.
        // Checking once here rather than per-test is what makes it unmissable: the loss flag is sticky, so the
        // run fails whichever invocable lost the device.
        // The poll is what makes it reliable -- a reset nothing has submitted against yet is invisible to the flag.
        auto& dx = static_cast<dx12::dx12_context&>(*ctx.value());
        dx.poll_device_removal();
        CHECK(!dx.is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}", dx.device_loss_reason()));
    }
}

static bool const sg_dx12_warp_registered = sg_test::register_backend("sg dx12 warp backend", "dx12-warp");
static bool const sg_dx12_hw_registered = sg_test::register_backend("sg dx12 hardware backend", "dx12-hw");
