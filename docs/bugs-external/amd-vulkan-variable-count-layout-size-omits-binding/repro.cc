// vkGetDescriptorSetLayoutSizeEXT of a layout whose last binding has VARIABLE_DESCRIPTOR_COUNT, on one vulkan device.
//
// The spec says the size counts that binding at its declared descriptorCount, its maximum:
// so it is at least the binding's offset plus descriptorCount times the descriptor's size.
// Each line is one layout of storage buffers: binding 0 holds one, binding 1 the count named.
//
//     repro.exe [--vendor <hex PCI id>]
//
// Prints the device, then per layout its size, binding 1's offset, and the least size the spec allows;
// exits 1 when a variable-count layout is smaller than that, 0 when none is, 2 when it could not run.

#define _CRT_SECURE_NO_WARNINGS
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK_VK(expr)                                                              \
    do                                                                              \
    {                                                                               \
        if (VkResult r_ = (expr); r_ != VK_SUCCESS)                                 \
        {                                                                           \
            std::printf("FAILED: %s returned %d (line %d)\n", #expr, r_, __LINE__); \
            std::exit(2);                                                           \
        }                                                                           \
    } while (0)

int main(int argc, char** argv)
{
    auto vendor = 0u;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--vendor") && i + 1 < argc)
            vendor = uint32_t(std::strtoul(argv[++i], nullptr, 16));

    auto const app = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    auto const instance_info = VkInstanceCreateInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    auto instance = VkInstance(VK_NULL_HANDLE);
    CHECK_VK(vkCreateInstance(&instance_info, nullptr, &instance));
    auto count = 0u;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    auto physicals = std::vector<VkPhysicalDevice>(count);
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, physicals.data()));
    auto physical = VkPhysicalDevice(VK_NULL_HANDLE);
    auto properties = VkPhysicalDeviceProperties{};
    for (auto const p : physicals)
    {
        vkGetPhysicalDeviceProperties(p, &properties);
        if (vendor == 0 ? properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : properties.vendorID == vendor)
        {
            physical = p;
            break;
        }
    }
    if (physical == VK_NULL_HANDLE && vendor == 0 && !physicals.empty())
        physical = physicals[0];
    if (physical == VK_NULL_HANDLE)
    {
        std::printf("FAILED: no device of vendor %04x\n", vendor);
        return 2;
    }
    auto db_properties = VkPhysicalDeviceDescriptorBufferPropertiesEXT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    auto properties2 = VkPhysicalDeviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &db_properties};
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    properties = properties2.properties;
    std::printf("device: %s, vendor %04x, driver %u.%u.%u (0x%08x), storage buffer descriptor %zu bytes\n",
                properties.deviceName, properties.vendorID, VK_API_VERSION_MAJOR(properties.driverVersion),
                VK_API_VERSION_MINOR(properties.driverVersion), VK_API_VERSION_PATCH(properties.driverVersion),
                properties.driverVersion, db_properties.storageBufferDescriptorSize);

    auto family = 0u;
    auto const priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                    .queueFamilyIndex = family,
                                                    .queueCount = 1,
                                                    .pQueuePriorities = &priority};
    auto descriptor_buffer_features = VkPhysicalDeviceDescriptorBufferFeaturesEXT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, .descriptorBuffer = VK_TRUE};
    auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .pNext = &descriptor_buffer_features,
                                                 .descriptorBindingPartiallyBound = VK_TRUE,
                                                 .descriptorBindingVariableDescriptorCount = VK_TRUE,
                                                 .bufferDeviceAddress = VK_TRUE};
    char const* const extensions[] = {VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME};
    auto const device_info = VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .pNext = &vk12,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue_info,
                                                .enabledExtensionCount = 1,
                                                .ppEnabledExtensionNames = extensions};
    auto device = VkDevice(VK_NULL_HANDLE);
    CHECK_VK(vkCreateDevice(physical, &device_info, nullptr, &device));
    auto const get_layout_size = PFN_vkGetDescriptorSetLayoutSizeEXT(vkGetDeviceProcAddr(device, "vkGetDescriptorSetLayoutSizeEXT"));
    auto const get_offset = PFN_vkGetDescriptorSetLayoutBindingOffsetEXT(vkGetDeviceProcAddr(device, "vkGetDescriptorSetLayoutBindingOffsetEXT"));

    auto const most = properties.limits.maxPerStageDescriptorStorageBuffers;
    uint32_t const counts[] = {4, 1024, most < 65536 ? most : 65536};
    auto reproduced = false;
    for (auto const is_variable : {false, true})
        for (auto const n : counts)
        {
            VkDescriptorSetLayoutBinding const bindings[] = {
                {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
                {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = n, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            };
            VkDescriptorBindingFlags const flags[] = {
                0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | (is_variable ? VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT : 0u)};
            auto const flags_info = VkDescriptorSetLayoutBindingFlagsCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 2, .pBindingFlags = flags};
            auto const info = VkDescriptorSetLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                              .pNext = &flags_info,
                                                              .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
                                                              .bindingCount = 2,
                                                              .pBindings = bindings};
            auto layout = VkDescriptorSetLayout(VK_NULL_HANDLE);
            CHECK_VK(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout));
            auto size = VkDeviceSize(0);
            auto offset = VkDeviceSize(0);
            get_layout_size(device, layout, &size);
            get_offset(device, layout, 1, &offset);
            auto const least = offset + VkDeviceSize(n) * db_properties.storageBufferDescriptorSize;
            auto const is_short = size < least;
            reproduced = reproduced || (is_variable && is_short);
            std::printf("%-8s count %5u: size %8llu, binding 1 at %4llu, least %8llu%s\n", is_variable ? "variable" : "fixed", n,
                        (unsigned long long)size, (unsigned long long)offset, (unsigned long long)least,
                        is_short ? "  SHORT" : "");
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::printf(reproduced ? "REPRODUCED: a variable-count layout is smaller than its binding's maximum count\n"
                           : "not reproduced: every layout holds its bindings at their declared counts\n");
    return reproduced ? 1 : 0;
}
