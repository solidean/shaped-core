#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

using namespace cc::primitive_defines;

// Workgroup memory's 64-bit atomics on whichever vulkan device has them, which need not be the one the sweep picks:
// an Intel Arc B580 lacks them, and an AMD Radeon iGPU beside it has them.

namespace sg_test
{
cc::shared_async<cc::unit> check_shared_sixty_four_bit_atomics(sg::context& ctx);
}

namespace
{
/// The first physical device with `shaderSharedInt64Atomics`, or none.
VkPhysicalDevice device_with_shared_int64_atomics(VkInstance instance)
{
    auto count = u32(0);
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    auto devices = cc::vector<VkPhysicalDevice>::create_defaulted(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    for (auto const device : devices)
    {
        auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        auto features = VkPhysicalDeviceFeatures2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vk12};
        vkGetPhysicalDeviceFeatures2(device, &features);
        if (vk12.shaderSharedInt64Atomics == VK_TRUE && vk12.shaderBufferInt64Atomics == VK_TRUE
            && features.features.shaderInt64 == VK_TRUE)
            return device;
    }
    return VK_NULL_HANDLE;
}
} // namespace

ASYNC_TEST("sg vulkan - workgroup 64-bit atomics on a device that has them (CHK-510)")
{
    auto ctx = sg::create_vulkan_context(
        {.enable_validation_layers = true,
         .choose_physical_device = [](VkInstance instance) { return device_with_shared_int64_atomics(instance); }});
    if (ctx.has_error())
        SKIP("no vulkan device has 64-bit atomics in workgroup memory");
    (void)sg_test::shader_fixtures(); // alive before any child acquires through it
    REQUIRE(ctx.value()->supports(sg::feature::shader_shared_int64_atomics));
    CC_LOG_INFO("device: {}", ctx.value()->metrics.adapter().name);
    co_await sg_test::check_shared_sixty_four_bit_atomics(*ctx.value());
    co_await ctx.value()->idle_completion();
}
