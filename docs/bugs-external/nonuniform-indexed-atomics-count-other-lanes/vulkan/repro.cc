// Vulkan alone: the dispatch ../repro.cc makes, of ../shader.hlsl compiled to SPIR-V by dxc.
// Usage: repro.exe shader.spv [--adapter <substring>]
// Bindings as dxc maps the registers: the two-element array at binding 0, `results` at binding 2, both in set 0.
// Prints the two words of each buffer; expected `even 32 0, odd 32 1`.
// Exits 0 when every word is the expected one, 1 when one is not, 2 when the run proves nothing.

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
constexpr uint32_t threads = 64;
constexpr uint32_t results_words = threads * 2;

VkDevice device = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties memory = {};

void check(VkResult result, char const* what)
{
    if (result == VK_SUCCESS)
        return;
    std::printf("error: %s (%d)\n", what, int(result));
    std::exit(2);
}

struct buffer
{
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t* mapped = nullptr;
};

// host-visible and coherent, so the host fills it and reads it back without a copy
buffer make_buffer(uint32_t words, uint32_t const* initial)
{
    auto out = buffer{};
    auto const info = VkBufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = words * 4,
                                         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    check(vkCreateBuffer(device, &info, nullptr, &out.handle), "vkCreateBuffer");
    auto req = VkMemoryRequirements{};
    vkGetBufferMemoryRequirements(device, out.handle, &req);
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
    check(vkAllocateMemory(device, &alloc, nullptr, &out.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, out.handle, out.memory, 0), "vkBindBufferMemory");
    check(vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&out.mapped)), "vkMapMemory");
    for (uint32_t i = 0; i < words; ++i)
        out.mapped[i] = initial != nullptr ? initial[i] : 0;
    return out;
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
        std::printf("usage: repro.exe shader.spv [--adapter <substring>]\n");
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

    // the first device whose name holds the substring, case-insensitively
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
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    auto subgroup = VkPhysicalDeviceSubgroupProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    auto properties2 = VkPhysicalDeviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    std::printf("adapter: %s, vulkan driver 0x%08x, subgroup %u\n", properties.deviceName, properties.driverVersion,
                subgroup.subgroupSize);
    std::fflush(stdout);

    auto family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    auto families = std::vector<VkQueueFamilyProperties>(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            family = i;

    // maximal reconvergence is enabled where the device has it, for a shader that asks for it by its execution mode
    auto extension_count = uint32_t(0);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
    auto extensions = std::vector<VkExtensionProperties>(extension_count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data());
    auto has_maximal = false;
    for (auto const& e : extensions)
        has_maximal = has_maximal || std::strcmp(e.extensionName, VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME) == 0;
    std::printf("maximal reconvergence: %s\n", has_maximal ? "yes" : "no");
    auto maximal = VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,
        .shaderMaximalReconvergence = VK_TRUE};
    auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .pNext = has_maximal ? &maximal : nullptr,
                                                 .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE};
    char const* const enabled[] = {VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME};
    auto const priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                    .queueFamilyIndex = family,
                                                    .queueCount = 1,
                                                    .pQueuePriorities = &priority};
    auto const device_info = VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .pNext = &vk12,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue_info,
                                                .enabledExtensionCount = has_maximal ? 1u : 0u,
                                                .ppEnabledExtensionNames = enabled};
    check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
    auto queue = VkQueue(VK_NULL_HANDLE);
    vkGetDeviceQueue(device, family, 0, &queue);

    uint32_t const start[2] = {0, 0xFFFFFFFFu};
    buffer const slots[2] = {make_buffer(2, start), make_buffer(2, start)};
    auto const results = make_buffer(results_words, nullptr);

    VkDescriptorSetLayoutBinding const bindings[] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
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
                  .pName = "main"},
        .layout = layout};
    auto pipeline = VkPipeline(VK_NULL_HANDLE);
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "vkCreateComputePipelines");

    auto const pool_size = VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3};
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
    VkDescriptorBufferInfo const slot_infos[] = {{.buffer = slots[0].handle, .range = VK_WHOLE_SIZE},
                                                 {.buffer = slots[1].handle, .range = VK_WHOLE_SIZE}};
    auto const results_info = VkDescriptorBufferInfo{.buffer = results.handle, .range = VK_WHOLE_SIZE};
    VkWriteDescriptorSet const writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = slot_infos},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &results_info},
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

    std::printf("even %u %u, odd %u %u\n", slots[0].mapped[0], slots[0].mapped[1], slots[1].mapped[0], slots[1].mapped[1]);
    std::fflush(stdout);
    auto const ok = slots[0].mapped[0] == threads / 2 && slots[0].mapped[1] == 0 && slots[1].mapped[0] == threads / 2
                 && slots[1].mapped[1] == 1;
    return ok ? 0 : 1;
}
