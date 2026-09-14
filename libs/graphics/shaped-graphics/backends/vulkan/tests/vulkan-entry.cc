#include "vulkan-test-common.hh"

#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>

// Entry-point driver for the vulkan backend suite (shaped-graphics-vulkan-test).
// It brings up ONE context and invokes every INVOCABLE_TEST in the binary against it, so the suite pays for one device rather than one per test.
// The context carries validation, sync validation and the fail-on-validation listener; see vulkan-test-common.hh.
//
// One device, not one per adapter: vulkan has no software adapter to guarantee, and the first hardware device is what the default run needs.
// A test that needs a context of its own — the context is its subject, or a vulkan_config knob is — stays an ordinary TEST and takes one from make_context.
//
// The invocables run serially on this context, so each leaves it clean: every list submitted or dropped, and any callback it swapped reinstalled.

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr char const* driver = "sg vulkan backend - device";
} // namespace

ASYNC_TEST("sg vulkan backend - device", exclusive("vulkan-device"))
{
    auto ctx = vulkan::test::make_context();
    if (ctx == nullptr)
        SKIP("no vulkan device");
    else
    {
        co_await nx::async_invoke_tests_in_sequence("device", ctx);

        // A device loss during our own tests is a defect, not an environment quirk to tolerate.
        // Vulkan has no equivalent of dx12's poll, so this sees only a loss some operation already noticed --
        // which every submitting test does.
        CHECK(!ctx->is_device_lost())
            .context(
                cc::format("the device was lost while running this binary's GPU tests: {}", ctx->device_loss_reason()));
    }
}

// One alias per invocable, so `dev.py test "sg vulkan - <name>"` still selects that one test.
NX_TEST_SETUP(nx::setup& s)
{
    auto const* const entry = s.find_test(driver);
    if (entry == nullptr)
        return;

    for (auto const* t : s.invocables_with<vulkan::vulkan_context_handle>())
    {
        cc::vector<nx::alias_fragment> fragments;
        fragments.push_back(nx::alias_fragment{.driver = entry, .section_path = {"device", t->name}});
        s.define_alias(t->name, cc::move(fragments));
    }
}
