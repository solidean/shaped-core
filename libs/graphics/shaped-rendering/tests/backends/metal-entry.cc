#include "../shader_fixtures.hh"
#include "sr_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

// metal entry-point driver for sr's GPU tests, compiled only where the metal backend builds, so on Apple platforms.
// Metal has no software device, so a host below the backend's floor cannot create a context and this SKIPs.
// Metal's validation has no callback to fail one test through: main() arms it for the whole binary instead.

// No exclusion tags, for the reason dx12-entry.cc gives.
ASYNC_TEST("sr metal")
{
    auto ctx = sg::create_metal_context({});
    if (ctx.has_error())
        SKIP("no metal 4 device");
    else
    {
        (void)sr_test::shader_fixtures(); // alive before any child acquires through it
        co_await nx::async_invoke_tests_in_sequence("metal", ctx.value());

        // A device loss during our own tests is a defect rather than an environment quirk to tolerate.
        CHECK(!ctx.value()->is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}",
                                ctx.value()->device_loss_reason()));
    }
}

static bool const sr_metal_registered = sr_test::register_backend("sr metal", "metal");
