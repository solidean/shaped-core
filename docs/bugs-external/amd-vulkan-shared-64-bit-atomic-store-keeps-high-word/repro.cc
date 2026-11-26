// Vulkan alone: one workgroup of 64 runs a shader.spvasm row that run.py assembled, and the host reads its 64-bit sum.
//
//     repro.exe <cs.spv> [--adapter <substring>]
//
// It runs on the first hardware device with the two features below, as shaped-core's test picks one.
// The device enables shaderInt64 and shaderSharedInt64Atomics, which the shader's workgroup sum needs.
// Prints one line: the adapter, the sum's two words, and whether they are 2^39 + 2016.
// Exit codes: 0 the sum is right, 1 it is wrong, 2 setup failed or no adapter matched.

#include <vulkan/vulkan.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
void check(VkResult result, char const* what)
{
    if (result == VK_SUCCESS)
        return;
    std::printf("error: %s (%d)\n", what, int(result));
    std::exit(2);
}

std::vector<char> read_file(char const* path)
{
    auto file = std::ifstream(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(file), {});
}

// 64 threads each add 2^33 plus an index from 0 to 63
constexpr auto expected = (uint64_t(64) << 33) + 63 * 64 / 2;
constexpr auto unwritten = 0xdeadbeefu;
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: repro.exe <cs.spv> [--adapter <substring>]\n");
        return 2;
    }
    auto adapter_match = std::string();
    for (auto i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--adapter") == 0 && i + 1 < argc)
            adapter_match = argv[++i];
    for (auto& c : adapter_match)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    auto const code = read_file(argv[1]);
    if (code.empty() || code.size() % 4 != 0)
        check(VK_ERROR_UNKNOWN, "no SPIR-V");

    auto const app = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    auto const instance_info = VkInstanceCreateInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    auto instance = VkInstance(VK_NULL_HANDLE);
    check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");

    // the first hardware device with the features whose name holds the substring, case-insensitively
    auto count = 0u;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    auto physicals = std::vector<VkPhysicalDevice>(count);
    check(vkEnumeratePhysicalDevices(instance, &count, physicals.data()), "vkEnumeratePhysicalDevices");
    auto physical = VkPhysicalDevice(VK_NULL_HANDLE);
    auto properties = VkPhysicalDeviceProperties{};
    for (auto const p : physicals)
    {
        vkGetPhysicalDeviceProperties(p, &properties);
        auto name = std::string(properties.deviceName);
        for (auto& c : name)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        auto supported12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        auto supported = VkPhysicalDeviceFeatures2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported12};
        vkGetPhysicalDeviceFeatures2(p, &supported);
        auto const has_features = supported.features.shaderInt64 && supported12.shaderSharedInt64Atomics;
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && has_features && name.find(adapter_match) != std::string::npos)
        {
            physical = p;
            break;
        }
    }
    if (physical == VK_NULL_HANDLE)
        check(VK_ERROR_FEATURE_NOT_PRESENT, "no hardware adapter with shaderInt64 and shaderSharedInt64Atomics matches --adapter");
    vkGetPhysicalDeviceProperties(physical, &properties);
    auto memory = VkPhysicalDeviceMemoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    auto family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    auto families = std::vector<VkQueueFamilyProperties>(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            family = i;

    auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .shaderSharedInt64Atomics = VK_TRUE};
    auto const features = VkPhysicalDeviceFeatures{.shaderInt64 = VK_TRUE};
    auto const priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                    .queueFamilyIndex = family,
                                                    .queueCount = 1,
                                                    .pQueuePriorities = &priority};
    auto const device_info = VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .pNext = &vk12,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue_info,
                                                .pEnabledFeatures = &features};
    auto device = VkDevice(VK_NULL_HANDLE);
    check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
    auto queue = VkQueue(VK_NULL_HANDLE);
    vkGetDeviceQueue(device, family, 0, &queue);

    // two words, host-visible and coherent, so the host reads them back without a copy
    auto const buffer_info = VkBufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                .size = 8,
                                                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    auto output = VkBuffer(VK_NULL_HANDLE);
    check(vkCreateBuffer(device, &buffer_info, nullptr, &output), "vkCreateBuffer");
    auto req = VkMemoryRequirements{};
    vkGetBufferMemoryRequirements(device, output, &req);
    auto const wanted = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
            type = i;
    if (type == UINT32_MAX)
        check(VK_ERROR_UNKNOWN, "no host-visible coherent memory");
    auto const alloc = VkMemoryAllocateInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                            .allocationSize = req.size,
                                            .memoryTypeIndex = type};
    auto buffer_memory = VkDeviceMemory(VK_NULL_HANDLE);
    check(vkAllocateMemory(device, &alloc, nullptr, &buffer_memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, output, buffer_memory, 0), "vkBindBufferMemory");
    auto* words = static_cast<uint32_t*>(nullptr);
    check(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&words)), "vkMapMemory");
    words[0] = unwritten;
    words[1] = unwritten;

    auto const binding = VkDescriptorSetLayoutBinding{.binding = 0,
                                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                      .descriptorCount = 1,
                                                      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    auto const set_layout_info = VkDescriptorSetLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                                 .bindingCount = 1,
                                                                 .pBindings = &binding};
    auto set_layout = VkDescriptorSetLayout(VK_NULL_HANDLE);
    check(vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
    auto const layout_info = VkPipelineLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                        .setLayoutCount = 1,
                                                        .pSetLayouts = &set_layout};
    auto layout = VkPipelineLayout(VK_NULL_HANDLE);
    check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout");

    auto const module_info = VkShaderModuleCreateInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                      .codeSize = code.size(),
                                                      .pCode = reinterpret_cast<uint32_t const*>(code.data())};
    auto module = VkShaderModule(VK_NULL_HANDLE);
    check(vkCreateShaderModule(device, &module_info, nullptr, &module), "vkCreateShaderModule");
    auto const pipeline_info = VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = layout};
    auto pipeline = VkPipeline(VK_NULL_HANDLE);
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "vkCreateComputePipelines");

    auto const pool_size = VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    auto const descriptor_pool_info = VkDescriptorPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                                 .maxSets = 1,
                                                                 .poolSizeCount = 1,
                                                                 .pPoolSizes = &pool_size};
    auto descriptor_pool = VkDescriptorPool(VK_NULL_HANDLE);
    check(vkCreateDescriptorPool(device, &descriptor_pool_info, nullptr, &descriptor_pool), "vkCreateDescriptorPool");
    auto const set_info = VkDescriptorSetAllocateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                      .descriptorPool = descriptor_pool,
                                                      .descriptorSetCount = 1,
                                                      .pSetLayouts = &set_layout};
    auto set = VkDescriptorSet(VK_NULL_HANDLE);
    check(vkAllocateDescriptorSets(device, &set_info, &set), "vkAllocateDescriptorSets");
    auto const descriptor = VkDescriptorBufferInfo{.buffer = output, .range = VK_WHOLE_SIZE};
    auto const write = VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = 0,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .pBufferInfo = &descriptor};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    auto const command_pool_info = VkCommandPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                           .queueFamilyIndex = family};
    auto command_pool = VkCommandPool(VK_NULL_HANDLE);
    check(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool), "vkCreateCommandPool");
    auto const cmd_info = VkCommandBufferAllocateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = command_pool,
                                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                      .commandBufferCount = 1};
    auto cmd = VkCommandBuffer(VK_NULL_HANDLE);
    check(vkAllocateCommandBuffers(device, &cmd_info, &cmd), "vkAllocateCommandBuffers");
    auto const begin = VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    auto const barrier = VkMemoryBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                         0, nullptr);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    auto const fence_info = VkFenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    auto fence = VkFence(VK_NULL_HANDLE);
    check(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
    auto const submit = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull), "vkWaitForFences");

    auto const got = (uint64_t(words[1]) << 32) | words[0];
    auto const right = got == expected;
    std::printf("%s: high 0x%08x low 0x%08x, %s\n", properties.deviceName, words[1], words[0],
                right ? "right" : words[0] == unwritten ? "never written" : "wrong, 2^39 + 2016 is high 0x00000080 low 0x000007e0");
    return right ? 0 : 1;
}
