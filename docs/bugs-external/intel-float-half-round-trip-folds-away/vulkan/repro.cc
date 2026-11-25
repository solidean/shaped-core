// Vulkan alone: the dispatch ../repro.cc makes, of ../shader.hlsl compiled to SPIR-V by the Vulkan SDK's dxc.
//
//     repro.exe <cs.spv> [--adapter <substring>]
//
// `input` is binding 0 and `output` binding 1 of set 0, as dxc maps t0 and u1.
// The device enables shaderFloat16, which the shader's halves need.
// Prints one line, repro-judge.hh's.
// Exit codes: 0 every round trip is a half, 1 one is not, 2 setup failed or no adapter matched.

#include "../repro-judge.hh"

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
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: repro.exe <cs.spv> [--adapter <substring>] [--entry <name>]\n");
        return 2;
    }
    auto adapter_match = std::string();
    // `cs` is run.py's; a module SGL wrote names its entry point as the source does
    auto entry = std::string("cs");
    for (auto i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--adapter") == 0 && i + 1 < argc)
            adapter_match = argv[++i];
        else if (std::strcmp(argv[i], "--entry") == 0 && i + 1 < argc)
            entry = argv[++i];
    }
    for (auto& c : adapter_match)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    auto const code = read_file(argv[1]);
    if (code.empty() || code.size() % 4 != 0)
        check(VK_ERROR_UNKNOWN, "no SPIR-V");

    auto const app = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    auto const instance_info = VkInstanceCreateInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    auto instance = VkInstance(VK_NULL_HANDLE);
    check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");

    // the first hardware device whose name holds the substring, case-insensitively
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
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && name.find(adapter_match) != std::string::npos)
        {
            physical = p;
            break;
        }
    }
    if (physical == VK_NULL_HANDLE)
        check(VK_ERROR_UNKNOWN, "no adapter matches --adapter");
    vkGetPhysicalDeviceProperties(physical, &properties);
    auto memory = VkPhysicalDeviceMemoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    auto supported12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    auto supported = VkPhysicalDeviceFeatures2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported12};
    vkGetPhysicalDeviceFeatures2(physical, &supported);
    if (!supported12.shaderFloat16)
        check(VK_ERROR_FEATURE_NOT_PRESENT, "the adapter has no shaderFloat16");

    auto family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    auto families = std::vector<VkQueueFamilyProperties>(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            family = i;

    auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .shaderFloat16 = VK_TRUE};
    auto const priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                    .queueFamilyIndex = family,
                                                    .queueCount = 1,
                                                    .pQueuePriorities = &priority};
    auto const device_info = VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .pNext = &vk12,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue_info};
    auto device = VkDevice(VK_NULL_HANDLE);
    check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
    auto queue = VkQueue(VK_NULL_HANDLE);
    vkGetDeviceQueue(device, family, 0, &queue);

    // host-visible and coherent, so the host fills it and reads it back without a copy
    auto const make_buffer = [&](uint32_t floats, VkBuffer& buffer) -> float*
    {
        auto const buffer_info = VkBufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                    .size = VkDeviceSize(floats) * 4,
                                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer), "vkCreateBuffer");
        auto req = VkMemoryRequirements{};
        vkGetBufferMemoryRequirements(device, buffer, &req);
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
        check(vkBindBufferMemory(device, buffer, buffer_memory, 0), "vkBindBufferMemory");
        auto* mapped = static_cast<float*>(nullptr);
        check(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&mapped)), "vkMapMemory");
        std::memset(mapped, 0, std::size_t(floats) * 4);
        return mapped;
    };
    auto const in = judge::corpus_inputs();
    auto input = VkBuffer(VK_NULL_HANDLE);
    auto output = VkBuffer(VK_NULL_HANDLE);
    std::memcpy(make_buffer(judge::inputs, input), in.data(), in.size() * sizeof(float));
    auto const* const got = make_buffer(judge::outputs, output);

    VkDescriptorSetLayoutBinding const bindings[] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    auto const set_layout_info = VkDescriptorSetLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                                 .bindingCount = 2,
                                                                 .pBindings = bindings};
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
                  .pName = entry.c_str()},
        .layout = layout};
    auto pipeline = VkPipeline(VK_NULL_HANDLE);
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "vkCreateComputePipelines");

    auto const pool_size = VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
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
    VkDescriptorBufferInfo const descriptors[] = {{.buffer = input, .range = VK_WHOLE_SIZE},
                                                  {.buffer = output, .range = VK_WHOLE_SIZE}};
    VkWriteDescriptorSet const writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &descriptors[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &descriptors[1]},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

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
    vkCmdDispatch(cmd, judge::inputs / 64, 1, 1);
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

    return judge::judged(in, got, properties.deviceName) ? 0 : 1;
}
