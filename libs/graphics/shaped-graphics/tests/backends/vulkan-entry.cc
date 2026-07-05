#include "../api/sg_test_backends.hh"

#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context

// vulkan entry-point driver inside the sg API test binary (shaped-graphics-test). Vulkan has no guaranteed
// software device, so a context can't be created on a driver-less headless host; the driver then SKIPs. When a
// device is present it invokes every sg::context_handle API test against it. Compiled only where the vulkan
// backend builds (the SDK is present).

TEST("sg vulkan backend")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    else
        nx::invoke_tests("vulkan", ctx.value());
}

static bool const sg_vulkan_backend_registered = sg_test::register_backend("sg vulkan backend", "vulkan");
