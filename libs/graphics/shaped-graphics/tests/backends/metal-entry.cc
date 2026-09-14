#include "sg_backends.hh"

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
// There is no validation listener here yet, and that is the gap worth closing next.
// Metal has no callback of the kind dx12 and vulkan install.
// Its API and shader validation layers are switched on by the MTL_DEBUG_LAYER / MTL_SHADER_VALIDATION environment variables, and they abort rather than calling back.
// MTLLogState carries a different channel again.
// Until that is settled, this driver has no oracle beyond the tests' own assertions.

// The same exclusions as the dx12 and vulkan drivers, for the same children: see dx12-entry.cc.
TEST("sg metal backend", nx::config::disabled, exclusive("slib-shader-library"), exclusive("sg-reload-generation"))
{
    auto ctx = sg::create_metal_context({});
    if (ctx.has_error())
        SKIP("no metal 4 device");
    else
        nx::invoke_tests("metal", ctx.value());
}

static bool const sg_metal_registered = sg_test::register_backend("sg metal backend", "metal");
