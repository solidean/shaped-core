// Runs shader.hlsl over one wave of 64 threads and checks every thread's result against the same computation on the CPU.
// Depends on the Vulkan loader and the C++ standard library alone.
//
//     repro --spirv <file> [--vendor <hex PCI id>] [--dump]
//
// Prints the device, then `wrong=<threads>`, and exits 0 once the dispatch completes; 2 on a setup failure.
// `--dump` also prints every thread's result, one hex word per line.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t value_count = 280; // N in shader.hlsl

uint32_t input_value(uint32_t i)
{
    return i * 0x9E3779B9u + 1;
}

// shader.hlsl's thread t, on the CPU.
uint32_t expected(uint32_t t)
{
    std::vector<uint32_t> v(value_count);
    for (uint32_t i = 0; i < value_count; ++i)
        v[i] = input_value(i);
    for (int m = 0; m < 2; ++m)
        for (uint32_t i = 0; i < value_count; ++i)
            v[i] = v[i] * 3 + (t ^ v[(i * 7) % value_count]);
    uint32_t acc = t;
    for (uint32_t i = 0; i < value_count; ++i)
        acc ^= v[i];
    return acc;
}

#define CHECK_VK(x)                                               \
    do                                                            \
    {                                                             \
        VkResult const r_ = (x);                                  \
        if (r_ != VK_SUCCESS)                                     \
        {                                                         \
            std::fprintf(stderr, "%s failed: %d\n", #x, int(r_)); \
            std::exit(2);                                         \
        }                                                         \
    } while (0)

uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits_allowed)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    auto const want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits_allowed & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    std::fprintf(stderr, "no host-visible coherent memory\n");
    std::exit(2);
}

struct buffer
{
    VkBuffer handle;
    VkDeviceMemory memory;
    void* mapped;
};

buffer make_buffer(VkDevice device, VkPhysicalDevice gpu, VkDeviceSize size)
{
    auto b = buffer{};
    auto const info = VkBufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = size,
                                         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CHECK_VK(vkCreateBuffer(device, &info, nullptr, &b.handle));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    auto const alloc = VkMemoryAllocateInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                            .allocationSize = req.size,
                                            .memoryTypeIndex = memory_type(gpu, req.memoryTypeBits)};
    CHECK_VK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
    CHECK_VK(vkBindBufferMemory(device, b.handle, b.memory, 0));
    CHECK_VK(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
    return b;
}
} // namespace

int main(int argc, char** argv)
{
    std::string spirv_path;
    long vendor = -1;
    bool dump = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--dump") == 0)
        {
            dump = true;
            continue;
        }
        if (i + 1 >= argc)
            break;
        if (std::strcmp(argv[i], "--spirv") == 0)
            spirv_path = argv[++i];
        else if (std::strcmp(argv[i], "--vendor") == 0)
            vendor = std::strtol(argv[++i], nullptr, 16);
    }
    std::ifstream file(spirv_path, std::ios::binary);
    if (spirv_path.empty() || !file)
    {
        std::fprintf(stderr, "usage: repro --spirv <file> [--vendor <hex>]\n");
        return 2;
    }
    std::vector<char> const code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto const app = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    auto const instance_info
        = VkInstanceCreateInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK_VK(vkCreateInstance(&instance_info, nullptr, &instance));

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props = {};
    for (auto const g : gpus)
    {
        vkGetPhysicalDeviceProperties(g, &props);
        if (vendor >= 0 ? long(props.vendorID) == vendor : props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
        {
            gpu = g;
            break;
        }
    }
    if (gpu == VK_NULL_HANDLE)
    {
        std::fprintf(stderr, "no matching device\n");
        return 2;
    }
    auto driver = VkPhysicalDeviceDriverProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    auto props2 = VkPhysicalDeviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(gpu, &props2);
    std::printf("device: %s, vendor %04x, %s %s\n", props.deviceName, props.vendorID, driver.driverName,
                driver.driverInfo);

    uint32_t family = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++family;
    float const priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                    .queueFamilyIndex = family,
                                                    .queueCount = 1,
                                                    .pQueuePriorities = &priority};
    auto const device_info = VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue_info};
    VkDevice device;
    CHECK_VK(vkCreateDevice(gpu, &device_info, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    auto const input = make_buffer(device, gpu, value_count * 4);
    for (uint32_t i = 0; i < value_count; ++i)
        static_cast<uint32_t*>(input.mapped)[i] = input_value(i);
    auto const output = make_buffer(device, gpu, 64 * 4);
    std::memset(output.mapped, 0, 64 * 4);

    VkDescriptorSetLayoutBinding const bindings[2] = {
        {.binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    auto const set_layout_info
        = VkDescriptorSetLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2,
                                          .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    CHECK_VK(vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout));
    auto const layout_info = VkPipelineLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                        .setLayoutCount = 1,
                                                        .pSetLayouts = &set_layout};
    VkPipelineLayout layout;
    CHECK_VK(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));

    auto const module_info = VkShaderModuleCreateInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                      .codeSize = code.size(),
                                                      .pCode = reinterpret_cast<uint32_t const*>(code.data())};
    VkShaderModule module;
    CHECK_VK(vkCreateShaderModule(device, &module_info, nullptr, &module));
    auto const pipeline_info
        = VkComputePipelineCreateInfo{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = layout};
    VkPipeline pipeline;
    CHECK_VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

    auto const pool_size = VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
    auto const pool_info = VkDescriptorPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                      .maxSets = 1,
                                                      .poolSizeCount = 1,
                                                      .pPoolSizes = &pool_size};
    VkDescriptorPool pool;
    CHECK_VK(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool));
    auto const set_info = VkDescriptorSetAllocateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                      .descriptorPool = pool,
                                                      .descriptorSetCount = 1,
                                                      .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    CHECK_VK(vkAllocateDescriptorSets(device, &set_info, &set));
    VkDescriptorBufferInfo const infos[2] = {{input.handle, 0, VK_WHOLE_SIZE}, {output.handle, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2];
    for (uint32_t i = 0; i < 2; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .pBufferInfo = &infos[i]};
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    auto const cmd_pool_info
        = VkCommandPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool cmd_pool;
    CHECK_VK(vkCreateCommandPool(device, &cmd_pool_info, nullptr, &cmd_pool));
    auto const cmd_info = VkCommandBufferAllocateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = cmd_pool,
                                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                      .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK_VK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    auto const begin = VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK_VK(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    auto const barrier = VkMemoryBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
    CHECK_VK(vkEndCommandBuffer(cmd));
    auto const submit
        = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK_VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK_VK(vkQueueWaitIdle(queue));

    auto const* out = static_cast<uint32_t const*>(output.mapped);
    std::string wrong;
    for (uint32_t t = 0; t < 64; ++t)
        if (out[t] != expected(t))
            wrong += (wrong.empty() ? "" : ",") + std::to_string(t);
    std::printf("wrong=%s\n", wrong.empty() ? "none" : wrong.c_str());
    if (dump)
        for (uint32_t t = 0; t < 64; ++t)
            std::printf("%08x\n", out[t]);
    vkDeviceWaitIdle(device);
    return 0;
}
