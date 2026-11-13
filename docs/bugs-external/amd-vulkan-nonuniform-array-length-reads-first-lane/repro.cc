// OpArrayLength of a binding array's element at a non-uniform index, on one vulkan device.
//
// The shader is length.spvasm: 4 lanes, lane x reading views[(x + shift) % 2], a view of 2 words and a view of 8.
// It writes each lane's OpArrayLength and the element's first word, so a right answer at shift 0 is
//     lengths=2,8,2,8 firsts=10,20,10,20
// and at shift 1, where the first lane reads the long view, lengths=8,2,8,2 firsts=20,10,20,10.
//
//     repro.exe --spirv <length.spv> (--buffer | --sets) [--shift 0|1] [--vendor <hex PCI id>]
//
// --buffer binds the two views through VK_EXT_descriptor_buffer, --sets through a descriptor set.
// Prints the device, then one line of results; exits 0 when the shader ran, whatever it answered, and 2 when it could not.

#define _CRT_SECURE_NO_WARNINGS
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK_VK(expr)                                                         \
    do                                                                         \
    {                                                                          \
        if (VkResult r_ = (expr); r_ != VK_SUCCESS)                            \
        {                                                                      \
            std::printf("FAILED: %s returned %d (line %d)\n", #expr, r_, __LINE__); \
            std::exit(2);                                                      \
        }                                                                      \
    } while (0)

namespace
{
VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties g_memory = {};

struct buffer_t
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
};

/// A host-visible, coherent buffer with a device address, filled with `data` where given.
buffer_t make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void const* data)
{
    auto out = buffer_t{};
    auto const info = VkBufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = size,
                                         .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    CHECK_VK(vkCreateBuffer(g_device, &info, nullptr, &out.buffer));
    auto req = VkMemoryRequirements{};
    vkGetBufferMemoryRequirements(g_device, out.buffer, &req);
    auto type = UINT32_MAX;
    auto const wanted = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < g_memory.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (g_memory.memoryTypes[i].propertyFlags & wanted) == wanted)
        {
            type = i;
            break;
        }
    if (type == UINT32_MAX)
    {
        std::printf("FAILED: no host-visible coherent memory\n");
        std::exit(2);
    }
    auto const flags = VkMemoryAllocateFlagsInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                 .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    auto const alloc = VkMemoryAllocateInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                            .pNext = &flags,
                                            .allocationSize = req.size,
                                            .memoryTypeIndex = type};
    CHECK_VK(vkAllocateMemory(g_device, &alloc, nullptr, &out.memory));
    CHECK_VK(vkBindBufferMemory(g_device, out.buffer, out.memory, 0));
    CHECK_VK(vkMapMemory(g_device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
    if (data != nullptr)
        std::memcpy(out.mapped, data, size_t(size));
    else
        std::memset(out.mapped, 0xff, size_t(size));
    auto const address = VkBufferDeviceAddressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out.buffer};
    out.address = vkGetBufferDeviceAddress(g_device, &address);
    return out;
}

std::vector<uint32_t> read_spirv(char const* path)
{
    auto* f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        std::printf("FAILED: cannot open %s\n", path);
        std::exit(2);
    }
    std::fseek(f, 0, SEEK_END);
    auto const size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    auto words = std::vector<uint32_t>(size_t(size) / 4);
    std::fread(words.data(), 4, words.size(), f);
    std::fclose(f);
    return words;
}
} // namespace

int main(int argc, char** argv)
{
    char const* spirv_path = nullptr;
    auto use_buffer = -1;
    auto vendor = 0u;
    auto shift = 0u;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--spirv") && i + 1 < argc)
            spirv_path = argv[++i];
        else if (!std::strcmp(argv[i], "--buffer"))
            use_buffer = 1;
        else if (!std::strcmp(argv[i], "--sets"))
            use_buffer = 0;
        else if (!std::strcmp(argv[i], "--shift") && i + 1 < argc)
            shift = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--vendor") && i + 1 < argc)
            vendor = uint32_t(std::strtoul(argv[++i], nullptr, 16));
    }
    if (spirv_path == nullptr || use_buffer < 0)
    {
        std::printf("usage: repro.exe --spirv <length.spv> (--buffer | --sets) [--shift 0|1] [--vendor <hex>]\n");
        return 2;
    }

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
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &g_memory);
    std::printf("device: %s, vendor %04x, driver %u.%u.%u (0x%08x), %s\n", properties.deviceName, properties.vendorID,
                VK_API_VERSION_MAJOR(properties.driverVersion), VK_API_VERSION_MINOR(properties.driverVersion),
                VK_API_VERSION_PATCH(properties.driverVersion), properties.driverVersion,
                use_buffer ? "descriptor buffer" : "descriptor set");
    std::printf("shift: %u\n", shift);

    auto family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    auto families = std::vector<VkQueueFamilyProperties>(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            family = i;
            break;
        }

    auto descriptor_buffer_features = VkPhysicalDeviceDescriptorBufferFeaturesEXT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, .descriptorBuffer = VK_TRUE};
    auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .pNext = use_buffer ? &descriptor_buffer_features : nullptr,
                                                 .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
                                                 .bufferDeviceAddress = VK_TRUE};
    auto const priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                    .queueFamilyIndex = family,
                                                    .queueCount = 1,
                                                    .pQueuePriorities = &priority};
    char const* const extensions[] = {VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME};
    auto const device_info = VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .pNext = &vk12,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue_info,
                                                .enabledExtensionCount = use_buffer ? 1u : 0u,
                                                .ppEnabledExtensionNames = extensions};
    CHECK_VK(vkCreateDevice(physical, &device_info, nullptr, &g_device));
    auto queue = VkQueue(VK_NULL_HANDLE);
    vkGetDeviceQueue(g_device, family, 0, &queue);

    uint32_t const short_words[] = {10, 11};
    uint32_t const long_words[] = {20, 21, 22, 23, 24, 25, 26, 27};
    auto const out = make_buffer(8 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr);
    auto const short_view = make_buffer(sizeof(short_words), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, short_words);
    auto const long_view = make_buffer(sizeof(long_words), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, long_words);

    VkDescriptorSetLayoutBinding const bindings[] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    auto const set_layout_info = VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = use_buffer ? VkDescriptorSetLayoutCreateFlags(VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) : 0u,
        .bindingCount = 2,
        .pBindings = bindings};
    auto set_layout = VkDescriptorSetLayout(VK_NULL_HANDLE);
    CHECK_VK(vkCreateDescriptorSetLayout(g_device, &set_layout_info, nullptr, &set_layout));
    auto const layout_info = VkPipelineLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                        .setLayoutCount = 1,
                                                        .pSetLayouts = &set_layout};
    auto layout = VkPipelineLayout(VK_NULL_HANDLE);
    CHECK_VK(vkCreatePipelineLayout(g_device, &layout_info, nullptr, &layout));

    auto const words = read_spirv(spirv_path);
    auto const module_info = VkShaderModuleCreateInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                      .codeSize = words.size() * 4,
                                                      .pCode = words.data()};
    auto module = VkShaderModule(VK_NULL_HANDLE);
    CHECK_VK(vkCreateShaderModule(g_device, &module_info, nullptr, &module));
    auto const shift_entry = VkSpecializationMapEntry{.constantID = 0, .offset = 0, .size = sizeof(shift)};
    auto const specialization = VkSpecializationInfo{.mapEntryCount = 1, .pMapEntries = &shift_entry,
                                                     .dataSize = sizeof(shift), .pData = &shift};
    auto const pipeline_info = VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = use_buffer ? VkPipelineCreateFlags(VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) : 0u,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main",
                  .pSpecializationInfo = &specialization},
        .layout = layout};
    auto pipeline = VkPipeline(VK_NULL_HANDLE);
    CHECK_VK(vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

    auto const pool_info = VkCommandPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    auto pool = VkCommandPool(VK_NULL_HANDLE);
    CHECK_VK(vkCreateCommandPool(g_device, &pool_info, nullptr, &pool));
    auto const cmd_info = VkCommandBufferAllocateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = pool,
                                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                      .commandBufferCount = 1};
    auto cmd = VkCommandBuffer(VK_NULL_HANDLE);
    CHECK_VK(vkAllocateCommandBuffers(g_device, &cmd_info, &cmd));
    auto const begin = VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK_VK(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    buffer_t descriptors;
    if (use_buffer)
    {
        auto const get_layout_size = PFN_vkGetDescriptorSetLayoutSizeEXT(vkGetDeviceProcAddr(g_device, "vkGetDescriptorSetLayoutSizeEXT"));
        auto const get_offset = PFN_vkGetDescriptorSetLayoutBindingOffsetEXT(vkGetDeviceProcAddr(g_device, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
        auto const get_descriptor = PFN_vkGetDescriptorEXT(vkGetDeviceProcAddr(g_device, "vkGetDescriptorEXT"));
        auto const bind_buffers = PFN_vkCmdBindDescriptorBuffersEXT(vkGetDeviceProcAddr(g_device, "vkCmdBindDescriptorBuffersEXT"));
        auto const set_offsets = PFN_vkCmdSetDescriptorBufferOffsetsEXT(vkGetDeviceProcAddr(g_device, "vkCmdSetDescriptorBufferOffsetsEXT"));
        auto db_properties = VkPhysicalDeviceDescriptorBufferPropertiesEXT{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        auto properties2 = VkPhysicalDeviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &db_properties};
        vkGetPhysicalDeviceProperties2(physical, &properties2);

        auto size = VkDeviceSize(0);
        get_layout_size(g_device, set_layout, &size);
        VkDeviceSize offsets[2] = {};
        get_offset(g_device, set_layout, 0, &offsets[0]);
        get_offset(g_device, set_layout, 1, &offsets[1]);
        auto const stride = db_properties.storageBufferDescriptorSize;
        descriptors = make_buffer(size < 256 ? 256 : size,
                                  VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
                                  nullptr);
        std::memset(descriptors.mapped, 0, size_t(size));
        auto const write = [&](buffer_t const& b, VkDeviceSize range, VkDeviceSize at)
        {
            auto const address = VkDescriptorAddressInfoEXT{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
                                                            .address = b.address,
                                                            .range = range};
            auto const info = VkDescriptorGetInfoEXT{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                                     .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                     .data = {.pStorageBuffer = &address}};
            get_descriptor(g_device, &info, stride, static_cast<char*>(descriptors.mapped) + at);
        };
        write(out, 8 * 4, offsets[0]);
        write(short_view, sizeof(short_words), offsets[1]);
        write(long_view, sizeof(long_words), offsets[1] + stride);
        std::printf("layout: size %llu, binding 1 at %llu, descriptor %zu bytes\n", (unsigned long long)size,
                    (unsigned long long)offsets[1], stride);

        auto const binding_info = VkDescriptorBufferBindingInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
            .address = descriptors.address,
            .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT};
        bind_buffers(cmd, 1, &binding_info);
        auto const index = 0u;
        auto const offset = VkDeviceSize(0);
        set_offsets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &index, &offset);
    }
    else
    {
        auto const pool_size = VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3};
        auto const descriptor_pool_info = VkDescriptorPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                                     .maxSets = 1,
                                                                     .poolSizeCount = 1,
                                                                     .pPoolSizes = &pool_size};
        auto descriptor_pool = VkDescriptorPool(VK_NULL_HANDLE);
        CHECK_VK(vkCreateDescriptorPool(g_device, &descriptor_pool_info, nullptr, &descriptor_pool));
        auto const set_info = VkDescriptorSetAllocateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                          .descriptorPool = descriptor_pool,
                                                          .descriptorSetCount = 1,
                                                          .pSetLayouts = &set_layout};
        auto set = VkDescriptorSet(VK_NULL_HANDLE);
        CHECK_VK(vkAllocateDescriptorSets(g_device, &set_info, &set));
        VkDescriptorBufferInfo const out_info = {.buffer = out.buffer, .offset = 0, .range = 8 * 4};
        VkDescriptorBufferInfo const view_infos[] = {{.buffer = short_view.buffer, .offset = 0, .range = sizeof(short_words)},
                                                     {.buffer = long_view.buffer, .offset = 0, .range = sizeof(long_words)}};
        VkWriteDescriptorSet const writes[] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &out_info},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 2,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = view_infos},
        };
        vkUpdateDescriptorSets(g_device, 2, writes, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    }

    vkCmdDispatch(cmd, 1, 1, 1);
    auto const barrier = VkMemoryBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    CHECK_VK(vkEndCommandBuffer(cmd));
    auto const fence_info = VkFenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    auto fence = VkFence(VK_NULL_HANDLE);
    CHECK_VK(vkCreateFence(g_device, &fence_info, nullptr, &fence));
    auto const submit = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK_VK(vkWaitForFences(g_device, 1, &fence, VK_TRUE, 10'000'000'000ull));

    auto const* const result = static_cast<uint32_t const*>(out.mapped);
    std::printf("lengths=%u,%u,%u,%u firsts=%u,%u,%u,%u\n", result[0], result[1], result[2], result[3], result[4],
                result[5], result[6], result[7]);
    return 0;
}
