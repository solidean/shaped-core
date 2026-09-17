#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_expected_messages.hh>

// Entry-point drivers for sr's GPU tests: each brings up ONE dx12 context and invokes every INVOCABLE_TEST taking an sg::context_handle against it.
// The adapter rules are libs/graphics/shaped-graphics/docs/testing.md, section "Devices and adapters":
//   - hardware: the real GPU, and the default; SKIPs when none is available, and FAILs when one is and creation still fails.
//   - WARP (software): the sweep on a host with no GPU, and a second pass under --thorough on one that has it.
//
// The drivers hold no exclusion tags: the async invocation takes each child's own around its run, and a driver holding them too would be refused.
// So a child that stands up a slib::shader_library, or an sr::imgui_context, carries that tag itself.
// Children under one driver run one after another on the same context, so each must leave it as it found it.

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr char const* warp_driver = "sr dx12 - warp";
constexpr char const* hardware_driver = "sr dx12 - hardware";

} // namespace

// The debug-layer advisories sg provokes on purpose (dx12_expected_messages.hh), allowed in every test of this binary.
// Validation fails a test through the log rule rather than through a listener, so anything else the layer says still fails it.
NX_ALLOW_LOGS(cc::rec::level::warning, "sg.dx12", sg::backend::dx12::k_expected_validation_messages);

ASYNC_TEST("sr dx12 - warp")
{
    // Beside a GPU, WARP is a second adapter the default run need not pay for; on a GPU-less host it is the only one.
    if (!nx::is_thorough() && dx12::has_hardware_adapter())
        SKIP("the hardware adapter covers the default run; WARP runs under --thorough");

    auto ctx = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::warp});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
    {
        co_await nx::async_invoke_tests_in_sequence("warp", ctx.value());

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

ASYNC_TEST("sr dx12 - hardware")
{
    auto ctx = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware});
    // A host that has the adapter and still cannot bring up a device is broken, and a SKIP would hide it.
    if (ctx.has_error() && dx12::has_hardware_adapter())
        FAIL(cc::format("dx12 hardware device creation failed: {}", ctx.error().to_string()));
    else if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
    {
        co_await nx::async_invoke_tests_in_sequence("hardware", ctx.value());

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

// One alias per invocable, so `dev.py test "sr - <name>"` still selects that one test, on both adapters.
NX_TEST_SETUP(nx::setup& s)
{
    auto const* const warp = s.find_test(warp_driver);
    auto const* const hardware = s.find_test(hardware_driver);

    for (auto const* t : s.invocables_with<sg::context_handle>())
    {
        cc::vector<nx::alias_fragment> fragments;
        if (warp != nullptr)
            fragments.push_back(nx::alias_fragment{.driver = warp, .section_path = {"warp", t->name}});
        if (hardware != nullptr)
            fragments.push_back(nx::alias_fragment{.driver = hardware, .section_path = {"hardware", t->name}});

        if (!fragments.empty())
            s.define_alias(t->name, cc::move(fragments));
    }
}
