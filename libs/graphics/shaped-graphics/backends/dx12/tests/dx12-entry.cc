#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>

// Entry-point drivers for the dx12 backend suite (shaped-graphics-dx12-test).
// Each brings up ONE context and invokes every INVOCABLE_TEST in the binary against it, so the suite costs two devices rather than one per test.
// Both carry the debug layer and the fail-on-validation listener; see dx12-test-common.hh.
//
// Two adapters, which is the point of having drivers at all:
//   - WARP (software): present on any Windows host, so the suite also runs headless on CI.
//   - hardware: the real GPU; SKIPs when none is available.
//
// A test that needs a context of its own — pristine pool/epoch state, or a backend knob — stays an ordinary TEST and takes one from make_test_context.

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr char const* warp_driver = "sg dx12 backend - warp";
constexpr char const* hardware_driver = "sg dx12 backend - hardware";
} // namespace

TEST("sg dx12 backend - warp")
{
    auto ctx = dx12::make_test_context();
    if (ctx.has_error())
        SKIP("no dx12 WARP device");
    else
        nx::invoke_tests("warp", ctx.value());
}

TEST("sg dx12 backend - hardware")
{
    auto ctx = dx12::as_test_context(sg::create_dx12_context({.enable_debug_layer = true, .use_warp = false}));
    if (ctx.has_error())
        SKIP("no dx12 hardware device");
    else
        nx::invoke_tests("hardware", ctx.value());
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
