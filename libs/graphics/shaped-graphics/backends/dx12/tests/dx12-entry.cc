#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>

// Entry-point drivers for the dx12 backend suite (shaped-graphics-dx12-test).
// Each brings up ONE context and invokes every INVOCABLE_TEST in the binary against it, so the suite costs one device per adapter that runs rather than one per test.
// Both carry the debug layer and the fail-on-validation listener; see dx12-test-common.hh.
//
// Two adapters, which is the point of having drivers at all:
//   - hardware: the real GPU; SKIPs when none is available, and FAILs when one is and creation still fails.
//   - WARP (software): the sweep on a host with no GPU, and a second pass under --thorough on one that has it.
//
// A test whose subject is the context itself — pristine pool/epoch state, a backend knob, two contexts — stays an ordinary TEST and takes one from make_test_context.

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr char const* warp_driver = "sg dx12 backend - warp";
constexpr char const* hardware_driver = "sg dx12 backend - hardware";
} // namespace

TEST("sg dx12 backend - warp")
{
    // Beside a GPU, WARP is a second adapter the default run need not pay for; on a GPU-less host it is the only one.
    if (!nx::is_thorough() && sg::backend::dx12::has_hardware_adapter())
        SKIP("the hardware adapter covers the default run; WARP runs under --thorough");

    auto ctx = dx12::as_test_context(
        sg::create_dx12_context({.enable_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::warp}));
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
    {
        // The driver is the one place that knows which adapter it asked for, so the flag is checked here rather than in a test.
        CHECK(ctx.value()->adapter().is_software);
        nx::invoke_tests("warp", ctx.value());
    }
}

TEST("sg dx12 backend - hardware")
{
    auto ctx = dx12::as_test_context(
        sg::create_dx12_context({.enable_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::hardware}));
    // A host that has the adapter and still cannot bring up a device is broken, and a SKIP would hide it.
    if (ctx.has_error() && dx12::has_hardware_adapter())
        FAIL(cc::format("dx12 hardware device creation failed: {}", ctx.error().to_string()));
    else if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
    {
        CHECK(!ctx.value()->adapter().is_software);
        nx::invoke_tests("hardware", ctx.value());
    }
}

// One alias per invocable, so `dev.py test "sg dx12 - <name>"` still selects that one test, on both adapters.
// The tier-1 binary does the same over a registry of backends (shaped-graphics/tests/backends/backends.cc); here the two drivers are the whole set.
NX_TEST_SETUP(nx::setup& s)
{
    auto const* const warp = s.find_test(warp_driver);
    auto const* const hardware = s.find_test(hardware_driver);

    for (auto const* t : s.invocables_with<dx12::dx12_context_handle>())
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
