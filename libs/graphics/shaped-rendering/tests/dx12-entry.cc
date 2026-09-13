#include <clean-core/string/format.hh>
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
// A child runs under its driver's config, so the drivers carry the exclusion tags the children need.
// The children stand up a slib::shader_library, a process-wide singleton, and the imgui ones an sr::imgui_context, which is another.
// Children under one driver run one after another on the same context, so each must leave it as it found it.

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr char const* warp_driver = "sr dx12 - warp";
constexpr char const* hardware_driver = "sr dx12 - hardware";

/// Fails whichever test provoked it on any debug-layer warning or worse, bar the advisories sg provokes on purpose.
/// Without this a validation error is a line on stderr nobody reads, and the run stays green.
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

TEST("sr dx12 - warp", exclusive("slib-shader-library"), exclusive("sr-imgui-context"))
{
    // Beside a GPU, WARP is a second adapter the default run need not pay for; on a GPU-less host it is the only one.
    if (!nx::is_thorough() && dx12::has_hardware_adapter())
        SKIP("the hardware adapter covers the default run; WARP runs under --thorough");

    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .adapter = dx12::dx12_adapter::warp});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
    {
        fail_on_validation_messages(ctx.value());
        nx::invoke_tests("warp", ctx.value());
    }
}

TEST("sr dx12 - hardware", exclusive("slib-shader-library"), exclusive("sr-imgui-context"))
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .adapter = dx12::dx12_adapter::hardware});
    // A host that has the adapter and still cannot bring up a device is broken, and a SKIP would hide it.
    if (ctx.has_error() && dx12::has_hardware_adapter())
        FAIL(cc::format("dx12 hardware device creation failed: {}", ctx.error().to_string()));
    else if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
    {
        fail_on_validation_messages(ctx.value());
        nx::invoke_tests("hardware", ctx.value());
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
