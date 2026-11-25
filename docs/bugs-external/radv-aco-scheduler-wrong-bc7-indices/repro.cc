// Runs shader.hlsl's BC7 encoder over 64 blocks, block p holding two exact ramps only partition p fits, and checks each
// block it writes by decoding it on the CPU.
// Depends on the Vulkan loader and the C++ standard library alone.
//
//     repro --spirv <file> [--vendor <hex PCI id>]
//
// Prints the device, then `bad=<blocks> other=<blocks>`, and exits 0 once the dispatch completes; 2 on a setup failure.
// `bad` is a block in mode 3 over its own partition that decodes wrong; `other` is a block in another mode or partition.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
// The 64 two-subset partitions as masks, bit i set where texel i (row-major) is in subset 1.
constexpr uint32_t partition_masks[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};
constexpr int anchors[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2, 2, 8,
                             8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8, 2, 2,
                             2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15};
constexpr int weights2[4] = {0, 21, 43, 64};

int lerp(int a, int b, int w)
{
    return ((64 - w) * a + w * b + 32) >> 6;
}

// 32 x 32 rgba8: block p holds partition p's subsets as two four-level ramps, one reddening and one bluing.
// Only partition p fits them exactly, so a mode 3 encode of block p decodes to within its endpoints' rounding.
std::vector<uint32_t> partition_ramps()
{
    std::vector<uint32_t> out(32 * 32, 0);
    for (int p = 0; p < 64; ++p)
    {
        int const ones = __builtin_popcount(partition_masks[p]);
        int const sizes[2] = {16 - ones, ones};
        int seen[2] = {0, 0};
        for (int i = 0; i < 16; ++i)
        {
            int const subset = (partition_masks[p] >> i) & 1;
            int const j = seen[subset]++;
            int level = j == 0 ? 0 : (j == sizes[subset] - 1 ? 3 : 1 + (j - 1) % 2);
            int const w = weights2[level];
            uint32_t const r = subset == 0 ? lerp(40, 200, w) : 120;
            uint32_t const g = subset == 0 ? 100 : 30;
            uint32_t const b = subset == 0 ? 60 : lerp(40, 220, w);
            out[((p / 8) * 4 + i / 4) * 32 + (p % 8) * 4 + i % 4] = r | g << 8 | b << 16 | 255u << 24;
        }
    }
    return out;
}

struct bits
{
    uint8_t const* data;
    int pos = 0;
    int take(int n)
    {
        int v = 0;
        for (int k = 0; k < n; ++k, ++pos)
            v |= ((data[pos >> 3] >> (pos & 7)) & 1) << k;
        return v;
    }
};

// Decodes a mode 3 block into rgb; false for any other mode, whose block this check does not judge.
bool decode_mode3(uint8_t const* block, int rgb[16][3], int* partition)
{
    auto b = bits{block};
    if (b.take(4) != 8)
        return false;
    int const p = b.take(6);
    int e[4][3];
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 4; ++k)
            e[k][c] = b.take(7);
    for (int k = 0; k < 4; ++k)
    {
        int const pbit = b.take(1);
        for (int c = 0; c < 3; ++c)
            e[k][c] = (e[k][c] << 1) | pbit;
    }
    for (int i = 0; i < 16; ++i)
    {
        int const index = b.take(i == 0 || i == anchors[p] ? 1 : 2);
        int const s = (partition_masks[p] >> i) & 1;
        for (int c = 0; c < 3; ++c)
            rgb[i][c] = lerp(e[2 * s][c], e[2 * s + 1][c], weights2[index]);
    }
    *partition = p;
    return true;
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
    for (int i = 1; i + 1 < argc; ++i)
    {
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

    auto const source = partition_ramps();
    auto const texels = make_buffer(device, gpu, source.size() * 4);
    std::memcpy(texels.mapped, source.data(), source.size() * 4);
    auto const blocks = make_buffer(device, gpu, 64 * 16);
    std::memset(blocks.mapped, 0, 64 * 16);

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
                                                .pName = "bc7_cs"},
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
    VkDescriptorBufferInfo const infos[2] = {{texels.handle, 0, VK_WHOLE_SIZE}, {blocks.handle, 0, VK_WHOLE_SIZE}};
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

    // A block is bad when it is mode 3 over its own partition and a texel decodes more than 2 from the source.
    // A block in any other mode, or over another partition, is a weaker encode rather than this bug, and is counted apart.
    auto const* out = static_cast<uint8_t const*>(blocks.mapped);
    std::string bad, other;
    for (int p = 0; p < 64; ++p)
    {
        int rgb[16][3];
        int partition = -1;
        if (!decode_mode3(out + p * 16, rgb, &partition) || partition != p)
        {
            other += (other.empty() ? "" : ",") + std::to_string(p);
            continue;
        }
        int worst = 0;
        for (int i = 0; i < 16; ++i)
        {
            uint32_t const t = source[((p / 8) * 4 + i / 4) * 32 + (p % 8) * 4 + i % 4];
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(rgb[i][c] - int((t >> (8 * c)) & 0xFF)));
        }
        if (worst > 2)
            bad += (bad.empty() ? "" : ",") + std::to_string(p);
    }
    std::printf("bad=%s other=%s\n", bad.empty() ? "none" : bad.c_str(), other.empty() ? "none" : other.c_str());
    vkDeviceWaitIdle(device);
    return 0;
}
