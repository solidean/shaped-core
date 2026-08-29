#include "sg_backends.hh"

#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context

// vulkan entry-point driver inside the sg API test binary (shaped-graphics-test).
// Vulkan has no guaranteed software device, so a context cannot be created on a driver-less headless host and the driver then SKIPs.
// When a device is present it invokes every sg::context_handle API test against it.
// Compiled only where the vulkan backend builds, so where the SDK is present.
//
// The backend is registered but its driver is disabled, and the two flags do different jobs while it is built out.
// Registering is what makes backends.cc build an alias per invocable, so `dev.py test "sg - <name>"` can run one API test against vulkan.
// nx::config::disabled keeps a full sweep out, because the recording paths vulkan has not reached yet abort rather than fail.
// Naming a test by its alias enables it deliberately, which is exactly the by-name run the build-out wants.
// Drop the disabled once no recording seam aborts.

TEST("sg vulkan backend", nx::config::disabled)
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    else
        nx::invoke_tests("vulkan", ctx.value());
}

static bool const sg_vulkan_registered = sg_test::register_backend("sg vulkan backend", "vulkan");
