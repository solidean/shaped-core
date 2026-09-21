#include "../shaders/shader_fixtures.hh"
#include "sg_backends.hh"

#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_context.hh> // sg::create_metal_context

// metal entry-point driver inside the sg API test binary (shaped-graphics-test).
// Metal has no software device, so a host below the backend's floor — a pre-26 OS, or a GPU outside the Metal 4 family —
// cannot create a context and the driver then SKIPs.
// Compiled only where the metal backend builds, so on Apple platforms.
//
// The driver runs the whole sweep, the way vulkan's does — it was `nx::config::disabled` while the backend was built
// out, and turning it on is what found the cross-list ordering defect no single test could reach.
// See libs/graphics/shaped-graphics/docs/writing-a-backend.md and libs/base/nexus/docs/invocable-tests.md.
//
// There is no listener installed here, and that is settled rather than pending.
// Metal has no validation callback of the kind dx12 and vulkan install: its messages go to stderr and nowhere else.
// The gate is instead armed in main() — see sg::backend::metal::arm_validation_layer — where a violation aborts the
// binary rather than failing one test.
// So this driver's oracle is process-wide, and coarser than the other two backends'.

// No exclusion tags, for the reason dx12-entry.cc gives.
ASYNC_TEST("sg metal backend")
{
    auto ctx = sg::create_metal_context({});
    if (ctx.has_error())
        SKIP("no metal 4 device");
    else
    {
        (void)sg_test::shader_fixtures(); // alive before any child acquires through it
        co_await nx::async_invoke_tests_in_sequence("metal", ctx.value());

        // A device loss during our own tests is a defect rather than an environment quirk to tolerate.
        CHECK(!ctx.value()->is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}",
                                ctx.value()->device_loss_reason()));
    }
}

static bool const sg_metal_registered = sg_test::register_backend("sg metal backend", "metal");
