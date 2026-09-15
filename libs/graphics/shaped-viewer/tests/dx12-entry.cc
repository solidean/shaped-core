#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>           // sg::create_dx12_context
#include <shaped-graphics/backends/dx12/dx12_expected_messages.hh> // the allowlist sg's suites share

// Entry-point drivers for sv's GPU tests.
// Each brings up ONE dx12 context and invokes every `INVOCABLE_TEST(..., (sg::context_handle const&))` in the binary against it.
// Both carry the debug layer and a listener that fails the running test on any validation warning.
//
// Two adapters, per libs/graphics/shaped-graphics/docs/testing.md#devices-and-adapters:
//   - hardware: the real GPU; SKIPs when none is available, and FAILs when one is and creation still fails.
//   - WARP (software): the sweep on a host with no GPU, and a second pass under --thorough on one that has it.
//
// The invocables run one after another on the one context, so each leaves it as it found it: no open command list, and nothing that outlives its test.
// A test whose subject is the context itself stays an ordinary TEST and creates its own.
//
// Both drivers carry capture-environment, because the invocables under them set the capture protocol's process environment.

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr char const* warp_driver = "sv dx12 - warp";
constexpr char const* hardware_driver = "sv dx12 - hardware";

/// Fails whichever test provoked it on any debug-layer warning or worse, bar the advisories sg provokes on purpose.
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

ASYNC_TEST("sv dx12 - warp", nx::config::exclusive("capture-environment"))
{
    // Beside a GPU, WARP is a second adapter the default run need not pay for; on a GPU-less host it is the only one.
    if (!nx::is_thorough() && sg::backend::dx12::has_hardware_adapter())
        SKIP("the hardware adapter covers the default run; WARP runs under --thorough");

    auto ctx = sg::create_dx12_context(
        {.activate_global_debug_layer = true, .enable_dred = true, .adapter = sg::backend::dx12::dx12_adapter::warp});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
    {
        fail_on_validation_messages(ctx.value());
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

ASYNC_TEST("sv dx12 - hardware", nx::config::exclusive("capture-environment"))
{
    auto ctx = sg::create_dx12_context(
        {.activate_global_debug_layer = true, .enable_dred = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    // A host that has the adapter and still cannot bring up a device is broken, and a SKIP would hide it.
    if (ctx.has_error() && dx12::has_hardware_adapter())
        FAIL(cc::format("dx12 hardware device creation failed: {}", ctx.error().to_string()));
    else if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
    {
        fail_on_validation_messages(ctx.value());
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

// One alias per invocable, so `dev.py test "sv - <name>"` still selects that one test, on both adapters.
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
