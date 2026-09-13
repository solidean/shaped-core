#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context

// Entry-point drivers for the whole-chain integration tests: compile HLSL → reflect → bind → run on a device → read back.
// Each brings up ONE context and invokes every INVOCABLE_TEST in the binary against it, so the suite costs two devices rather than one per test.
// Several concurrently live WARP devices are what the old shape produced at -jN, and WARP itself faulted under it.
//
// Two adapters:
//   - hardware: the real GPU; SKIPs when none is available.
//   - WARP (software): the sweep on a host with no GPU, and a second pass under --thorough on one that has it.
//
// The windowed tests build their own context: they are nx::config::manual, run one at a time by hand, and want a real adapter.

namespace
{
constexpr char const* warp_driver = "ssc::dxc + dx12 - warp backend";
constexpr char const* hardware_driver = "ssc::dxc + dx12 - hardware backend";
} // namespace

TEST("ssc::dxc + dx12 - warp backend")
{
    // Beside a GPU, WARP is a second adapter the default run need not pay for; on a GPU-less host it is the only one.
    if (!nx::is_thorough() && sg::backend::dx12::has_hardware_adapter())
        SKIP("the hardware adapter covers the default run; WARP runs under --thorough");

    auto ctx = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::warp});
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
        nx::invoke_tests("warp", ctx.value());
}

TEST("ssc::dxc + dx12 - hardware backend")
{
    auto ctx = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
        nx::invoke_tests("hardware", ctx.value());
}

// One alias per invocable, so `dev.py test "ssc::dxc + dx12 - <name>"` still selects that one test, on both adapters.
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
