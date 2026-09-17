#include "sg_backends.hh"

#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_context.hh> // sg::create_metal_context

// metal entry-point driver inside the sg API test binary (shaped-graphics-test).
// Metal has no software device, so a host below the backend's floor — a pre-26 OS, or a GPU outside the Metal 4 family —
// cannot create a context and the driver then SKIPs.
// Compiled only where the metal backend builds, so on Apple platforms.
//
// The driver runs `nx::config::disabled` while the backend is built out: every resource and recording seam still
// aborts, and a full sweep across them is a crash that takes the suite with it rather than a set of failures.
// Registering is what still buys something — it builds the per-invocable aliases, so any one API test runs against
// metal by being named exactly, which is how the suite is used as an oracle while the backend grows.
// See libs/graphics/shaped-graphics/docs/writing-a-backend.md and libs/base/nexus/docs/invocable-tests.md.
//
// Take the disabled off once no seam aborts, the way vulkan's came off.
//
// There is no listener installed here, and that is settled rather than pending.
// Metal has no validation callback of the kind dx12 and vulkan install: its messages go to stderr and nowhere else.
// The gate is instead armed in main() — see sg::backend::metal::arm_validation_layer — where a violation aborts the
// binary rather than failing one test.
// So this driver's oracle is process-wide, and coarser than the other two backends'.

// No exclusion tags, for the reason dx12-entry.cc gives.
// **Threaded builds only.**
// With SC_THREADS=OFF the sweep aborts before any test reports: metal settles its transfer completions from the
// MTL4CommitFeedback handler, on a queue Apple owns, and `cc::async`'s single-threaded scheduler refuses to wait on a
// node pushed from a thread it does not drive — "parked on an external push".
// That is a property of this backend's completion routing rather than of any one test, so it is pinned at the preset
// rather than per test; see libs/graphics/shaped-graphics/docs/TODO.md.
#if CC_HAS_THREADS

ASYNC_TEST("sg metal backend")
{
    auto ctx = sg::create_metal_context({});
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

#else // !CC_HAS_THREADS

// Registered but disabled, rather than absent: the registration is what makes every tier-1 invocable reachable by
// name, and nexus's orphan check exempts an alias-reachable invocable for exactly this case.
// Removing the driver instead orphans the whole tier-1 suite in this build.
TEST("sg metal backend", nx::config::disabled)
{
    SKIP("the metal sweep needs threads — see libs/graphics/shaped-graphics/docs/TODO.md");
}

#endif // CC_HAS_THREADS

static bool const sg_metal_registered = sg_test::register_backend("sg metal backend", "metal");
