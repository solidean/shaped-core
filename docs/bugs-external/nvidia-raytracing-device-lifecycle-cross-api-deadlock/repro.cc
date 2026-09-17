// NVIDIA driver deadlock between device lifecycle and ray-tracing work, within and across Vulkan and D3D12.
//
// Two threads, each running one role in a loop:
//   vk            create + destroy a VkInstance and a VkDevice with the ray-tracing extensions enabled
//   vk-plain      the same without the ray-tracing extensions
//   vk-sizes      vk, plus vkGetAccelerationStructureBuildSizesKHR on the device before destroying it
//   vk-pipeline   vk, plus one ray-tracing pipeline built on the device before destroying it
//   dx            create + release an ID3D12Device
//   dx-prebuild   dx, plus GetRaytracingAccelerationStructurePrebuildInfo before releasing it
//
//   repro --a vk --b dx-prebuild        the cross-API hang shaped-graphics-test ran into
//
// Build and run it through run.py.
// Exit code 0 = ran to the end, 1 = hung (reproduced), 2 = setup failed.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "vulkan-1.lib")

namespace
{
std::atomic<bool> g_stop = {false};
IDXGIAdapter1* g_adapter = nullptr;

// ---------------------------------------------------------------------------------------------------------------
// Vulkan
// ---------------------------------------------------------------------------------------------------------------

// `void main() {}` as a raygen shader, glslangValidator --target-env vulkan1.2.
constexpr uint32_t k_raygen_spirv[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x00000006, 0x00000000, 0x00020011, 0x0000117f, 0x0006000a,
    0x5f565053, 0x5f52484b, 0x5f796172, 0x63617274, 0x00676e69, 0x0006000b, 0x00000001, 0x4c534c47,
    0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x000014c1,
    0x00000004, 0x6e69616d, 0x00000000, 0x00030003, 0x00000002, 0x000001cc, 0x00060004, 0x455f4c47,
    0x725f5458, 0x745f7961, 0x69636172, 0x0000676e, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
};

void build_sizes(VkDevice device)
{
    auto const fn = reinterpret_cast<void (*)(VkDevice, VkAccelerationStructureBuildTypeKHR,
                                              VkAccelerationStructureBuildGeometryInfoKHR const*, uint32_t const*,
                                              VkAccelerationStructureBuildSizesInfoKHR*)>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    VkAccelerationStructureGeometryKHR geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    geometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
    VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    uint32_t const primitives = 1;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    fn(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
}

void build_pipeline(VkDevice device)
{
    auto const fn = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = sizeof(k_raygen_spirv);
    mci.pCode = k_raygen_spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &mci, nullptr, &module);
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &lci, nullptr, &layout);

    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stage.module = module;
    stage.pName = "main";
    VkRayTracingShaderGroupCreateInfoKHR group = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = 0;
    group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
    VkRayTracingPipelineCreateInfoKHR pci = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    pci.stageCount = 1;
    pci.pStages = &stage;
    pci.groupCount = 1;
    pci.pGroups = &group;
    pci.maxPipelineRayRecursionDepth = 1;
    pci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (fn(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) == VK_SUCCESS)
        vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
}

bool vk_round(bool raytracing, bool sizes, bool pipeline)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
        return false;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (auto d : devices)
    {
        VkPhysicalDeviceProperties p = {};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            physical = d;
    }
    if (physical == VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    float const priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    char const* const extensions[] = {
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,     VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,      VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    rt.rayTracingPipeline = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    as.accelerationStructure = VK_TRUE;
    as.pNext = &rt;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    bda.bufferDeviceAddress = VK_TRUE;
    bda.pNext = &as;

    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (raytracing)
    {
        dci.pNext = &bda;
        dci.enabledExtensionCount = uint32_t(sizeof(extensions) / sizeof(extensions[0]));
        dci.ppEnabledExtensionNames = extensions;
    }

    VkDevice device = VK_NULL_HANDLE;
    auto const ok = vkCreateDevice(physical, &dci, nullptr, &device) == VK_SUCCESS;
    if (ok)
    {
        if (sizes)
            build_sizes(device);
        if (pipeline)
            build_pipeline(device);
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// D3D12
// ---------------------------------------------------------------------------------------------------------------

IDXGIAdapter1* find_hardware_adapter()
{
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return nullptr;
    IDXGIAdapter1* found = nullptr;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        a->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            found = a;
            break;
        }
        a->Release();
    }
    factory->Release();
    return found;
}

bool dx_round(bool prebuild)
{
    ID3D12Device5* device = nullptr;
    if (FAILED(D3D12CreateDevice(g_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return false;
    if (prebuild)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometry.AABBs.AABBCount = 1;
        geometry.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    }
    device->Release();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------------------------------------------

bool run_role(std::string const& role)
{
    if (role == "vk")
        return vk_round(true, false, false);
    if (role == "vk-plain")
        return vk_round(false, false, false);
    if (role == "vk-sizes")
        return vk_round(true, true, false);
    if (role == "vk-pipeline")
        return vk_round(true, false, true);
    if (role == "dx")
        return dx_round(false);
    if (role == "dx-prebuild")
        return dx_round(true);
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    auto roles = std::vector<std::string>{"vk", "dx-prebuild"};
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (std::strcmp(argv[i], "--a") == 0)
            roles[0] = argv[i + 1];
        else if (std::strcmp(argv[i], "--b") == 0)
            roles[1] = argv[i + 1];
    }
    std::printf("a=%s b=%s\n", roles[0].c_str(), roles[1].c_str());

    g_adapter = find_hardware_adapter();
    if (g_adapter == nullptr)
    {
        std::printf("no hardware D3D12 adapter\n");
        return 2;
    }
    for (auto const& r : roles)
        if (!run_role(r))
        {
            std::printf("setup round failed for role '%s'\n", r.c_str());
            return 2;
        }

    std::atomic<long long> rounds[2] = {0, 0};
    auto threads = std::vector<std::thread>();
    for (int t = 0; t < 2; ++t)
        threads.emplace_back([&, t] {
            while (!g_stop.load() && run_role(roles[t]))
                rounds[t].fetch_add(1);
        });

    // Run for 15 seconds, or until neither thread has finished a round for 5.
    auto const start = std::chrono::steady_clock::now();
    auto last_progress = start;
    auto last = 0LL;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(15))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto const total = rounds[0].load() + rounds[1].load();
        if (total != last)
        {
            last = total;
            last_progress = std::chrono::steady_clock::now();
        }
        if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(5))
        {
            std::printf("HUNG after %lld (a) and %lld (b) rounds\n", rounds[0].load(), rounds[1].load());
            std::fflush(stdout);
            // The threads are stuck inside the driver, so joining them would hang the report as well.
            TerminateProcess(GetCurrentProcess(), 1);
        }
    }

    g_stop.store(true);
    for (auto& t : threads)
        t.join();
    std::printf("OK: %lld (a) and %lld (b) rounds\n", rounds[0].load(), rounds[1].load());
    g_adapter->Release();
    return 0;
}
