#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>
#include <shaped-graphics/backends/metal/metal_context.hh> // sg::create_metal_context

// metal entry-point driver for sv's GPU tests, compiled only where the metal backend builds, so on Apple platforms.
// It brings up ONE metal context and invokes every `INVOCABLE_TEST(..., (sg::context_handle const&))` in the binary against it.
// Metal has no software device, so a host below the backend's floor cannot create a context and this SKIPs.
// Metal's validation has no callback to fail one test through: main() arms it for the whole binary instead.
//
// It carries capture-environment for the reason dx12-entry.cc gives.

namespace
{
constexpr char const* metal_driver = "sv metal";
} // namespace

ASYNC_TEST("sv metal", nx::config::exclusive("capture-environment"))
{
    auto ctx = sg::create_metal_context();
    if (ctx.has_error())
        SKIP("no metal 4 device");
    else
    {
        co_await nx::async_invoke_tests_in_sequence("metal", ctx.value());

        // A device loss during our own tests is a defect rather than an environment quirk to tolerate.
        CHECK(!ctx.value()->is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}",
                                ctx.value()->device_loss_reason()));
    }
}

// One alias per invocable, so `dev.py test "sv - <name>"` still selects that one test.
NX_TEST_SETUP(nx::setup& s)
{
    auto const* const metal = s.find_test(metal_driver);
    if (metal == nullptr)
        return;

    for (auto const* t : s.invocables_with<sg::context_handle>())
    {
        cc::vector<nx::alias_fragment> fragments;
        fragments.push_back(nx::alias_fragment{.driver = metal, .section_path = {"metal", t->name}});
        s.define_alias(t->name, cc::move(fragments));
    }
}
