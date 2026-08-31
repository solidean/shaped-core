// Concurrent VkDevice create/destroy deadlocks, with or without other work in flight.
//
// Standalone.
// No shaped-core, no third-party headers: <vulkan/vulkan.h>, the C++ standard library, and nothing else.
// The whole point is that nothing of ours is in the picture, so a hang here is the loader's or the driver's.
//
// Build and run it through run.py, or by hand:
//   cl /O2 /std:c++20 /EHsc /I"%VULKAN_SDK%\Include" repro.cc /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib
//
// Exit code 0 = every thread finished, 1 = timed out (reproduced), 2 = no usable Vulkan device.

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
// ---------------------------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------------------------

struct options
{
    int threads = 8;             // how many threads churn devices
    int iterations = 6;          // create/destroy rounds per thread
    int timeout_seconds = 60;    // how long before we call it a hang
    bool validation = false;     // load the Khronos validation layer, as our test suite does
    bool pipelines = false;      // also build a compute pipeline on each device before destroying it
    bool raytracing = false;     // build a RAY-TRACING pipeline instead, which is the call that actually hangs
    int device_index = 0;        // which physical device, so one machine can compare two vendors
};

std::atomic<int> g_completed_threads = {0};
std::atomic<bool> g_failed = {false};

std::mutex g_print_mutex;

void note(std::string const& line)
{
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------------------------
// A minimal compute pipeline, so the churn can overlap real driver work.
//
// The SPIR-V below is `void main() {}` at local_size 1 — the smallest valid compute module.
// It is spelled as words rather than fetched or compiled, so this file stays the only input.
// ---------------------------------------------------------------------------------------------------------------

constexpr uint32_t k_empty_compute_spirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000006, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
};

// The raygen module, from:
//     #version 460
//     #extension GL_EXT_ray_tracing : require
//     void main() {}
// compiled with the SDK's glslangValidator at --target-env vulkan1.2, then spelled out here so this file stays the
// only input.
// An empty body is deliberate: what is being timed is the driver entering pipeline creation at all.
constexpr uint32_t k_empty_raygen_spirv[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x00000006, 0x00000000, 0x00020011, 0x0000117f, 0x0006000a,
    0x5f565053, 0x5f52484b, 0x5f796172, 0x63617274, 0x00676e69, 0x0006000b, 0x00000001, 0x4c534c47,
    0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x000014c1,
    0x00000004, 0x6e69616d, 0x00000000, 0x00030003, 0x00000002, 0x000001cc, 0x00060004, 0x455f4c47,
    0x725f5458, 0x745f7961, 0x69636172, 0x0000676e, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
};

// The device extensions a ray-tracing pipeline needs, plus what they in turn require.
char const* const k_raytracing_extensions[] = {
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
};

// Builds and tears down one ray-tracing pipeline on `device`.
// This is the call the hang was observed inside: the stack had one thread in nvoglv64.dll under
// vkCreateRayTracingPipelinesKHR while two dozen others waited on the loader's lock in create/destroy.
void build_a_raytracing_pipeline(VkDevice device)
{
    auto const create_rt_pipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    if (create_rt_pipelines == nullptr)
        return;

    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(k_empty_raygen_spirv);
    module_info.pCode = k_empty_raygen_spirv;

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS)
        return;

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) == VK_SUCCESS)
    {
        VkPipelineShaderStageCreateInfo stage = {};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        stage.module = module;
        stage.pName = "main";

        VkRayTracingShaderGroupCreateInfoKHR group = {};
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        group.generalShader = 0;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingPipelineCreateInfoKHR pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        pipeline_info.stageCount = 1;
        pipeline_info.pStages = &stage;
        pipeline_info.groupCount = 1;
        pipeline_info.pGroups = &group;
        pipeline_info.maxPipelineRayRecursionDepth = 1;
        pipeline_info.layout = layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (create_rt_pipelines(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline)
            == VK_SUCCESS)
            vkDestroyPipeline(device, pipeline, nullptr);

        vkDestroyPipelineLayout(device, layout, nullptr);
    }

    vkDestroyShaderModule(device, module, nullptr);
}

// Builds and tears down one compute pipeline on `device`.
// Pipeline creation is where a driver does its shader compilation, which is the long call the churn overlaps with.
void build_a_pipeline(VkDevice device)
{
    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(k_empty_compute_spirv);
    module_info.pCode = k_empty_compute_spirv;

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS)
        return;

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) == VK_SUCCESS)
    {
        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) == VK_SUCCESS)
            vkDestroyPipeline(device, pipeline, nullptr);

        vkDestroyPipelineLayout(device, layout, nullptr);
    }

    vkDestroyShaderModule(device, module, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// One create / use / destroy round, which is what a per-test context costs.
// ---------------------------------------------------------------------------------------------------------------

bool one_round(options const& opt, int thread_index, int iteration)
{
    char const* const validation_layer = "VK_LAYER_KHRONOS_validation";

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vk-device-churn";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (opt.validation)
    {
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &validation_layer;
    }

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
    {
        note("thread " + std::to_string(thread_index) + ": vkCreateInstance failed");
        return false;
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0)
    {
        vkDestroyInstance(instance, nullptr);
        note("thread " + std::to_string(thread_index) + ": no physical device");
        return false;
    }

    std::vector<VkPhysicalDevice> physical_devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data());
    if (opt.device_index >= int(device_count))
    {
        vkDestroyInstance(instance, nullptr);
        note("no physical device at index " + std::to_string(opt.device_index));
        return false;
    }
    VkPhysicalDevice const physical = physical_devices[size_t(opt.device_index)];

    // Any queue family will do; the device is never submitted to.
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());

    uint32_t family = 0;
    for (uint32_t i = 0; i < family_count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            family = i;
            break;
        }

    float const priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    // The feature chain a ray-tracing pipeline needs.
    // Declared out here so it outlives the create call.
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_features = {};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features = {};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda_features = {};
    if (opt.raytracing)
    {
        rt_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rt_features.rayTracingPipeline = VK_TRUE;
        as_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        as_features.accelerationStructure = VK_TRUE;
        as_features.pNext = &rt_features;
        bda_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        bda_features.bufferDeviceAddress = VK_TRUE;
        bda_features.pNext = &as_features;

        device_info.pNext = &bda_features;
        device_info.enabledExtensionCount = uint32_t(std::size(k_raytracing_extensions));
        device_info.ppEnabledExtensionNames = k_raytracing_extensions;
    }

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS)
    {
        vkDestroyInstance(instance, nullptr);
        note("thread " + std::to_string(thread_index) + ": vkCreateDevice failed");
        return false;
    }

    if (opt.raytracing)
        build_a_raytracing_pipeline(device);
    else if (opt.pipelines)
        build_a_pipeline(device);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    (void)iteration;
    return true;
}

// Names the device under test and says whether it can do ray tracing at all.
// Without this an adapter with no ray-tracing support reports a clean OK, which reads as "does not reproduce" when
// what actually happened is that the interesting call never ran.
bool describe_selected_device(options const& opt)
{
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
        return false;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    if (opt.device_index >= int(count))
    {
        std::printf("device index %d out of range (%u present)\n", opt.device_index, count);
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    VkPhysicalDevice const physical = devices[size_t(opt.device_index)];
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(physical, &props);

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, exts.data());

    bool has_rt = false;
    for (auto const& e : exts)
        if (std::strcmp(e.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0)
            has_rt = true;

    std::printf("device %d of %u: %s (driver %u.%u.%u, ray tracing %s)\n", opt.device_index, count, props.deviceName,
                props.driverVersion >> 22, (props.driverVersion >> 14) & 0xff, (props.driverVersion >> 6) & 0xff,
                has_rt ? "yes" : "NO");
    std::fflush(stdout);

    vkDestroyInstance(instance, nullptr);

    if (opt.raytracing && !has_rt)
    {
        std::printf("this device has no ray-tracing pipelines, so --raytracing would prove nothing here\n");
        return false;
    }
    return true;
}

void worker(options const& opt, int thread_index)
{
    for (int i = 0; i < opt.iterations; ++i)
        if (!one_round(opt, thread_index, i))
        {
            g_failed.store(true);
            break;
        }

    g_completed_threads.fetch_add(1);
}

int parse_int(char const* value, int fallback)
{
    char* end = nullptr;
    long const parsed = std::strtol(value, &end, 10);
    return (end != nullptr && *end == '\0') ? int(parsed) : fallback;
}
} // namespace

int main(int argc, char** argv)
{
    options opt;
    for (int i = 1; i < argc; ++i)
    {
        std::string const arg = argv[i];
        if (arg == "--threads" && i + 1 < argc)
            opt.threads = parse_int(argv[++i], opt.threads);
        else if (arg == "--iterations" && i + 1 < argc)
            opt.iterations = parse_int(argv[++i], opt.iterations);
        else if (arg == "--timeout" && i + 1 < argc)
            opt.timeout_seconds = parse_int(argv[++i], opt.timeout_seconds);
        else if (arg == "--validation")
            opt.validation = true;
        else if (arg == "--pipelines")
            opt.pipelines = true;
        else if (arg == "--raytracing")
            opt.raytracing = true;
        else if (arg == "--device" && i + 1 < argc)
            opt.device_index = parse_int(argv[++i], opt.device_index);
        else
        {
            std::printf("usage: %s [--threads N] [--iterations N] [--timeout S] [--validation] [--pipelines] [--raytracing] [--device N]\n",
                        argv[0]);
            return 2;
        }
    }

    std::printf("threads=%d iterations=%d validation=%s pipelines=%s raytracing=%s timeout=%ds\n", opt.threads,
                opt.iterations, opt.validation ? "on" : "off", opt.pipelines ? "on" : "off",
                opt.raytracing ? "on" : "off", opt.timeout_seconds);
    std::fflush(stdout);

    if (!describe_selected_device(opt))
        return 2;

    // One probe round on the main thread, so "no Vulkan here" is distinguishable from "it hung".
    if (!one_round(opt, -1, 0))
    {
        std::printf("no usable Vulkan device - nothing to test\n");
        return 2;
    }

    std::vector<std::thread> workers;
    workers.reserve(size_t(opt.threads));
    for (int i = 0; i < opt.threads; ++i)
        workers.emplace_back(worker, opt, i);

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opt.timeout_seconds);
    while (g_completed_threads.load() < opt.threads && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int const finished = g_completed_threads.load();
    if (finished < opt.threads)
    {
        std::printf("HUNG: %d of %d threads finished within %ds\n", finished, opt.threads, opt.timeout_seconds);
        std::printf("Attach a debugger to see the stacks; the process is deliberately left alive.\n");
        std::fflush(stdout);

        // Deliberately not joined: the threads are stuck inside the loader, and joining would hang the report too.
        // The harness is what kills the process.
        std::quick_exit(1);
    }

    for (auto& t : workers)
        t.join();

    if (g_failed.load())
    {
        std::printf("a round failed for a reason other than a hang - see above\n");
        return 2;
    }

    std::printf("OK: all %d threads completed %d rounds each\n", opt.threads, opt.iterations);
    return 0;
}
