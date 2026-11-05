#include "vulkan-test-common.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

using namespace cc::primitive_defines;

// A context created to a spec someone else dictates: the hooks an XR runtime creates the instance and device through,
// and the extensions it adds.
// Every test owns its context, since creation is its subject.

namespace
{
namespace vulkan = sg::backend::vulkan;

[[nodiscard]] cc::span<char const* const> enabled_names(char const* const* names, u32 count)
{
    return cc::span<char const* const>(names, isize(count));
}

[[nodiscard]] int count_name(cc::span<char const* const> names, cc::string_view wanted)
{
    auto n = 0;
    for (auto const* name : names)
        if (cc::string_view(name) == wanted)
            ++n;
    return n;
}

/// The first physical device of `instance`, which is what a runtime hands back from its own choice.
[[nodiscard]] VkPhysicalDevice first_physical_device(VkInstance instance)
{
    auto count = u32(0);
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0)
        return VK_NULL_HANDLE;
    auto devices = cc::vector<VkPhysicalDevice>::create_uninitialized(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    return devices[0];
}
} // namespace

TEST("sg vulkan - the creation hooks create what the context then runs on", exclusive("vulkan-device"))
{
    auto created_instance = VkInstance(VK_NULL_HANDLE);
    auto chosen_device = VkPhysicalDevice(VK_NULL_HANDLE);
    auto created_device = VkDevice(VK_NULL_HANDLE);
    auto instance_api_version = u32(0);

    auto const ctx = vulkan::test::make_context({
        .enable_validation_layers = true,
        .create_instance =
            [&](VkInstanceCreateInfo const& info, VkInstance& out)
        {
            instance_api_version = info.pApplicationInfo->apiVersion;
            auto const r = vkCreateInstance(&info, nullptr, &out);
            created_instance = out;
            return r;
        },
        .choose_physical_device =
            [&](VkInstance instance)
        {
            CHECK(instance == created_instance);
            chosen_device = first_physical_device(instance);
            return chosen_device;
        },
        .create_device =
            [&](VkPhysicalDevice physical_device, VkDeviceCreateInfo const& info, VkDevice& out)
        {
            CHECK(physical_device == chosen_device);
            auto const r = vkCreateDevice(physical_device, &info, nullptr, &out);
            created_device = out;
            return r;
        },
    });
    if (ctx == nullptr)
        SKIP("no vulkan device, or its first device is not one sg can run on");

    CHECK(instance_api_version == vulkan::required_api_version);
    CHECK(ctx->instance() == created_instance);
    CHECK(ctx->physical_device() == chosen_device);
    CHECK(ctx->device() == created_device);
    CHECK(ctx->queue() != VK_NULL_HANDLE);

    // The context runs on what the hooks made like on its own.
    auto buf = ctx->create_vulkan_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());
    auto cmd = ctx->create_vulkan_command_list();
    REQUIRE(cmd.has_value());
    ctx->submit_vulkan_command_list(cc::move(cmd.value()));
}

TEST("sg vulkan - an extra extension sg enables already is enabled once", exclusive("vulkan-device"))
{
    auto instance_surface_count = -1;
    auto device_swapchain_count = -1;

    auto const ctx = vulkan::test::make_context({
        .enable_validation_layers = true,
        .extra_instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME},
        .extra_device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME},
        .create_instance =
            [&](VkInstanceCreateInfo const& info, VkInstance& out)
        {
            instance_surface_count = count_name(enabled_names(info.ppEnabledExtensionNames, info.enabledExtensionCount),
                                                VK_KHR_SURFACE_EXTENSION_NAME);
            return vkCreateInstance(&info, nullptr, &out);
        },
        .create_device =
            [&](VkPhysicalDevice physical_device, VkDeviceCreateInfo const& info, VkDevice& out)
        {
            device_swapchain_count = count_name(enabled_names(info.ppEnabledExtensionNames, info.enabledExtensionCount),
                                                VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            return vkCreateDevice(physical_device, &info, nullptr, &out);
        },
    });
    if (ctx == nullptr)
        SKIP("no vulkan device, or one without the surface and swapchain extensions");

    CHECK(instance_surface_count == 1);
    CHECK(device_swapchain_count == 1);
}

TEST("sg vulkan - an extra extension nobody offers fails creation, naming it", exclusive("vulkan-device"))
{
    auto const instance
        = sg::create_vulkan_context({.extra_instance_extensions = {"VK_SG_test_not_an_instance_extension"}});
    REQUIRE(instance.has_error());
    CHECK(instance.error().to_string().contains("VK_SG_test_not_an_instance_extension"));

    auto const device = sg::create_vulkan_context({.extra_device_extensions = {"VK_SG_test_not_a_device_extension"}});
    REQUIRE(device.has_error());
    // A host with no device fails before the extension is ever asked about.
    if (device.error().to_string().contains("no Vulkan device"))
        SKIP("no vulkan device");
    CHECK(device.error().to_string().contains("VK_SG_test_not_a_device_extension"));
}

TEST("sg vulkan - choosing no physical device fails creation", exclusive("vulkan-device"))
{
    auto const ctx = sg::create_vulkan_context({.choose_physical_device = [](VkInstance) { return VK_NULL_HANDLE; }});
    REQUIRE(ctx.has_error());
    CHECK(ctx.error().to_string().contains("choose_physical_device"));
}

TEST("sg vulkan - the adapter reports its LUID where the platform has one", exclusive("vulkan-device"))
{
    auto const ctx = vulkan::test::make_context();
    if (ctx == nullptr)
        SKIP("no vulkan device");

#ifdef CC_OS_WINDOWS
    CHECK(ctx->metrics.adapter().luid.has_value());
#else
    // Vulkan reports a LUID only where the platform defines one, and only Windows does.
    CHECK(!ctx->metrics.adapter().luid.has_value());
#endif
}
