#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context

// vulkan entry-point driver inside the sg API test binary (shaped-graphics-test). Vulkan has no guaranteed
// software device, so a context can't be created on a driver-less headless host; the driver then SKIPs. When a
// device is present it invokes every sg::context_handle API test against it. Compiled only where the vulkan
// backend builds (the SDK is present).
//
// Disabled + unregistered for now: the vulkan backend is stubbed (its epoch/transfer ops abort), so it must
// not run in a sweep (disabled) and must not be aliased into the per-invocable runs (unregistered — otherwise
// naming an sg API test by its alias, which enables disabled tests, would dispatch into the stub and abort).
// When the backend is real, restore the register_backend call below and drop nx::config::disabled.

TEST("sg vulkan backend", nx::config::disabled)
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    else
        nx::invoke_tests("vulkan", ctx.value());
}
