#include "../shader_fixtures.hh"
#include "sr_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

// vulkan entry-point driver for sr's GPU tests, compiled only where the vulkan backend builds.
// Vulkan has no guaranteed software device, so a driver-less headless host cannot create a context and this SKIPs.
// Validation runs with synchronization validation on, as sg's own sweep does: it is the oracle that sees a hazard between two submissions.

// No exclusion tags, for the reason dx12-entry.cc gives.
ASYNC_TEST("sr vulkan")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true, .enable_sync_validation = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    else
    {
        (void)sr_test::shader_fixtures(); // alive before any child acquires through it
        co_await nx::async_invoke_tests_in_sequence("vulkan", ctx.value());

        // A device loss during our own tests is a defect, not an environment quirk to tolerate.
        CHECK(!ctx.value()->is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}",
                                ctx.value()->device_loss_reason()));
    }
}

static bool const sr_vulkan_registered = sr_test::register_backend("sr vulkan", "vulkan");
