// Times the first acceleration-structure builds of a fresh process, on the system D3D12 runtime or the Agility SDK's.
//
// Standalone: D3D12, DXGI, the Agility SDK's headers and runtime, the Vulkan headers and loader, and the C++ standard
// library, nothing else.
// The Vulkan arm is compiled only where <vulkan/vulkan.h> is found; the loader is loaded at run time.
// Every build is one triangle and is waited on with a fence, so each time printed is the driver's alone.
//
//     repro.exe [--vendor <hex>] [--load <dll>] [--agility <dir>] [--dx12 <op>]... [--vulkan <op>]...
//
// `--vendor <hex>` takes the first adapter of that PCI vendor on both APIs, `1002` for AMD, instead of the high-performance one.
//
// `--load <dll>` only LoadLibrary's <dll> before anything else runs, and calls nothing in it.
// `--modules` prints, at the end, every loaded module whose name starts with `ig` or `d3d12`.
// `--agility <dir>` makes the D3D12 device through the Agility SDK whose D3D12Core.dll is in <dir>.
// An <op> is one of:
//   configuration  with --agility and as the first op only: get the SDK configuration interface, and stop there
//   factory     with --agility and as the first op only: load the Agility runtime's device factory, and make no device
//   none        create the device and nothing else
//   release     release the device and everything made on it
//   prebuild    ask the device for a BLAS's prebuild info
//   blas        build one triangle BLAS, PREFER_FAST_TRACE
//   blas-fast   the same with PREFER_FAST_BUILD
//   blas-update the same with ALLOW_UPDATE
//   tlas        build one TLAS over one instance of the last BLAS (or a null instance if none was built)
// Ops run in order, on one device per API, and the dx12 ones run first.
// Prints `name=<seconds>` per phase, then the adapter's name.
// Exit codes: 0 every op ran, 2 setup failed.

// clang-format off
// must precede d3d12.h: it defines the Agility headers' CLSID_D3D12SDKConfiguration, which the system d3d12.lib lacks
#include <initguid.h>
// clang-format on
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <wrl/client.h>

#if __has_include(<vulkan/vulkan.h>)
#define REPRO_VULKAN 1
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#else
#define REPRO_VULKAN 0
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
/// The PCI vendor `--vendor` asked for, 0 where it asked for none.
unsigned g_vendor = 0;

#define CHECK_HR(expr)                                                                   \
    do                                                                                   \
    {                                                                                    \
        if (HRESULT hr_ = (expr); FAILED(hr_))                                           \
        {                                                                                \
            std::printf("FAILED %s: 0x%08lx\n", #expr, static_cast<unsigned long>(hr_)); \
            return false;                                                                \
        }                                                                                \
    } while (false)

#define CHECK_VK(expr)                                                   \
    do                                                                   \
    {                                                                    \
        if (VkResult r_ = (expr); r_ != VK_SUCCESS)                      \
        {                                                                \
            std::printf("FAILED %s: %d\n", #expr, static_cast<int>(r_)); \
            return false;                                                \
        }                                                                \
    } while (false)

using clock_type = std::chrono::steady_clock;

double seconds_since(clock_type::time_point start)
{
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

void report(char const* api, std::string const& name, double s)
{
    std::printf("%s.%s=%.3f ", api, name.c_str(), s);
    std::fflush(stdout);
}

float const triangle[] = {0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, -0.5f, -0.5f, 0.0f};

// --- D3D12 -----------------------------------------------------------------------------------------------------------

D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags)
{
    auto d = D3D12_RESOURCE_DESC{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

struct dx12
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12DeviceFactory> device_factory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    std::vector<ComPtr<ID3D12Resource>> keep;
    D3D12_GPU_VIRTUAL_ADDRESS last_blas = 0;

    ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        auto props = D3D12_HEAP_PROPERTIES{.Type = heap};
        auto desc = buffer_desc(size, flags);
        ComPtr<ID3D12Resource> r;
        if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
            return nullptr;
        keep.push_back(r);
        return r;
    }

    ComPtr<ID3D12Resource> upload(void const* data, UINT64 size)
    {
        auto r = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!r)
            return nullptr;
        void* p = nullptr;
        r->Map(0, nullptr, &p);
        std::memcpy(p, data, size);
        r->Unmap(0, nullptr);
        return r;
    }

    bool wait()
    {
        CHECK_HR(queue->Signal(fence.Get(), ++fence_value));
        CHECK_HR(fence->SetEventOnCompletion(fence_value, nullptr));
        return true;
    }
};

bool create_dx12(dx12& d, char const* agility_dir, bool configuration_only, bool factory_only)
{
    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    if (g_vendor == 0)
        CHECK_HR(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&d.adapter)));
    else
        for (UINT i = 0; factory->EnumAdapters1(i, &d.adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            auto desc = DXGI_ADAPTER_DESC1{};
            d.adapter->GetDesc1(&desc);
            if (desc.VendorId == g_vendor)
                break;
            d.adapter = nullptr;
        }
    if (!d.adapter)
    {
        std::printf("FAILED: no adapter of vendor %04x\n", g_vendor);
        return false;
    }

    auto const start = clock_type::now();
    if (agility_dir != nullptr)
    {
        ComPtr<ID3D12SDKConfiguration1> configuration;
        CHECK_HR(D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration)));
        if (configuration_only)
        {
            report("dx12", "configuration", seconds_since(start));
            return true;
        }
        CHECK_HR(configuration->CreateDeviceFactory(D3D12_SDK_VERSION, agility_dir, IID_PPV_ARGS(&d.device_factory)));
        if (factory_only)
        {
            report("dx12", "factory", seconds_since(start));
            return true;
        }
        CHECK_HR(d.device_factory->CreateDevice(d.adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&d.device)));
    }
    else
        CHECK_HR(D3D12CreateDevice(d.adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&d.device)));
    report("dx12", "device", seconds_since(start));

    auto options5 = D3D12_FEATURE_DATA_D3D12_OPTIONS5{};
    CHECK_HR(d.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
    {
        std::printf("FAILED: the adapter has no ray tracing\n");
        return false;
    }

    auto queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK_HR(d.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&d.queue)));
    CHECK_HR(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.allocator)));
    CHECK_HR(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&d.list)));
    CHECK_HR(d.list->Close());
    CHECK_HR(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)));
    return true;
}

bool dx12_build(dx12& d, std::string const& op)
{
    auto const blas = op.rfind("blas", 0) == 0;
    auto geometry = D3D12_RAYTRACING_GEOMETRY_DESC{};
    auto inputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{};
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    ComPtr<ID3D12Resource> source;
    if (blas)
    {
        source = d.upload(triangle, sizeof(triangle));
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = 3;
        geometry.Triangles.VertexBuffer = {.StartAddress = source->GetGPUVirtualAddress(), .StrideInBytes = 12};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.pGeometryDescs = &geometry;
        inputs.Flags = op == "blas-fast"   ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                     : op == "blas-update" ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                                           : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    }
    else
    {
        auto instance = D3D12_RAYTRACING_INSTANCE_DESC{};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
        instance.InstanceMask = 0xff;
        instance.AccelerationStructure = d.last_blas;
        source = d.upload(&instance, sizeof(instance));
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.InstanceDescs = source->GetGPUVirtualAddress();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    }
    if (!source)
        return false;

    auto start = clock_type::now();
    auto info = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO{};
    d.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    report("dx12", op + ".prebuild", seconds_since(start));
    if (op == "prebuild")
        return true;

    auto const uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto result = d.buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto scratch = d.buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON);
    if (!result || !scratch)
        return false;

    start = clock_type::now();
    CHECK_HR(d.allocator->Reset());
    CHECK_HR(d.list->Reset(d.allocator.Get(), nullptr));
    auto build = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    d.list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    CHECK_HR(d.list->Close());
    report("dx12", op + ".record", seconds_since(start));

    start = clock_type::now();
    ID3D12CommandList* lists[] = {d.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    if (!d.wait())
        return false;
    report("dx12", op + ".execute", seconds_since(start));
    if (blas)
        d.last_blas = result->GetGPUVirtualAddress();
    return d.device->GetDeviceRemovedReason() == S_OK;
}

#if REPRO_VULKAN
// --- Vulkan ----------------------------------------------------------------------------------------------------------

#define VK_FUNCTIONS(X)                           \
    X(vkCreateInstance)                           \
    X(vkEnumeratePhysicalDevices)                 \
    X(vkGetPhysicalDeviceProperties)              \
    X(vkGetPhysicalDeviceMemoryProperties)        \
    X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkCreateDevice)                             \
    X(vkGetDeviceQueue)                           \
    X(vkCreateBuffer)                             \
    X(vkGetBufferMemoryRequirements)              \
    X(vkAllocateMemory)                           \
    X(vkBindBufferMemory)                         \
    X(vkMapMemory)                                \
    X(vkGetBufferDeviceAddress)                   \
    X(vkCreateCommandPool)                        \
    X(vkAllocateCommandBuffers)                   \
    X(vkBeginCommandBuffer)                       \
    X(vkEndCommandBuffer)                         \
    X(vkResetCommandPool)                         \
    X(vkCreateFence)                              \
    X(vkResetFences)                              \
    X(vkWaitForFences)                            \
    X(vkQueueSubmit)                              \
    X(vkGetAccelerationStructureBuildSizesKHR)    \
    X(vkCreateAccelerationStructureKHR)           \
    X(vkGetAccelerationStructureDeviceAddressKHR) \
    X(vkCmdBuildAccelerationStructuresKHR)

#define VK_DECLARE(name) PFN_##name name = nullptr;
VK_FUNCTIONS(VK_DECLARE)

struct vk
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory = {};
    VkDeviceAddress last_blas = 0;

    struct buffer_t
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
    };

    bool buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, void const* data, buffer_t& out)
    {
        auto info = VkBufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = size,
                                       .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
        CHECK_VK(vkCreateBuffer(device, &info, nullptr, &out.buffer));
        auto req = VkMemoryRequirements{};
        vkGetBufferMemoryRequirements(device, out.buffer, &req);
        auto const want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        auto type = UINT32_MAX;
        for (auto i = 0u; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want)
                type = i;
        auto flags = VkMemoryAllocateFlagsInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                               .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        auto alloc = VkMemoryAllocateInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                          .pNext = &flags,
                                          .allocationSize = req.size,
                                          .memoryTypeIndex = type};
        CHECK_VK(vkAllocateMemory(device, &alloc, nullptr, &out.memory));
        CHECK_VK(vkBindBufferMemory(device, out.buffer, out.memory, 0));
        if (data != nullptr)
        {
            void* p = nullptr;
            CHECK_VK(vkMapMemory(device, out.memory, 0, size, 0, &p));
            std::memcpy(p, data, size);
        }
        auto address
            = VkBufferDeviceAddressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out.buffer};
        out.address = vkGetBufferDeviceAddress(device, &address);
        return true;
    }
};

bool create_vk(vk& v, std::wstring const& adapter_name)
{
    auto loader = LoadLibraryW(L"vulkan-1.dll");
    if (loader == nullptr)
    {
        std::printf("FAILED: no vulkan-1.dll\n");
        return false;
    }
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));

    auto const start = clock_type::now();
    auto app = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    auto instance_info = VkInstanceCreateInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK_VK(vkCreateInstance(&instance_info, nullptr, &v.instance));
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(get(v.instance, #name));
    VK_FUNCTIONS(VK_LOAD)

    // the physical device with the dx12 adapter's name, or the first discrete one
    auto count = 0u;
    CHECK_VK(vkEnumeratePhysicalDevices(v.instance, &count, nullptr));
    auto physicals = std::vector<VkPhysicalDevice>(count);
    CHECK_VK(vkEnumeratePhysicalDevices(v.instance, &count, physicals.data()));
    auto narrow = std::string(adapter_name.begin(), adapter_name.end());
    for (auto p : physicals)
    {
        auto props = VkPhysicalDeviceProperties{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (g_vendor != 0 ? props.vendorID == g_vendor && v.physical == VK_NULL_HANDLE
                          : narrow == props.deviceName
                                || (v.physical == VK_NULL_HANDLE && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
            v.physical = p;
    }
    if (v.physical == VK_NULL_HANDLE)
    {
        std::printf("FAILED: no vulkan device\n");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(v.physical, &v.memory);

    auto families = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(v.physical, &families, nullptr);
    auto family_props = std::vector<VkQueueFamilyProperties>(families);
    vkGetPhysicalDeviceQueueFamilyProperties(v.physical, &families, family_props.data());
    auto family = 0u;
    while (family < families && !(family_props[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++family;

    auto priority = 1.0f;
    auto queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = family,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
    auto as_features = VkPhysicalDeviceAccelerationStructureFeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .accelerationStructure = VK_TRUE};
    auto v12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                .pNext = &as_features,
                                                .bufferDeviceAddress = VK_TRUE};
    char const* extensions[]
        = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    auto device_info = VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                          .pNext = &v12,
                                          .queueCreateInfoCount = 1,
                                          .pQueueCreateInfos = &queue_info,
                                          .enabledExtensionCount = 2,
                                          .ppEnabledExtensionNames = extensions};
    CHECK_VK(vkCreateDevice(v.physical, &device_info, nullptr, &v.device));
    report("vulkan", "device", seconds_since(start));

    vkGetDeviceQueue(v.device, family, 0, &v.queue);
    auto pool_info
        = VkCommandPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    CHECK_VK(vkCreateCommandPool(v.device, &pool_info, nullptr, &v.pool));
    auto cmd_info = VkCommandBufferAllocateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = v.pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
    CHECK_VK(vkAllocateCommandBuffers(v.device, &cmd_info, &v.cmd));
    auto fence_info = VkFenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK_VK(vkCreateFence(v.device, &fence_info, nullptr, &v.fence));
    return true;
}

bool vk_build(vk& v, std::string const& op)
{
    auto const blas = op.rfind("blas", 0) == 0;
    auto const input_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    auto source = vk::buffer_t{};
    auto geometry = VkAccelerationStructureGeometryKHR{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                                       .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
    auto inputs = VkAccelerationStructureBuildGeometryInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry};
    auto primitives = 1u;
    if (blas)
    {
        if (!v.buffer(sizeof(triangle), input_usage, true, triangle, source))
            return false;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                       .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                       .vertexData = {.deviceAddress = source.address},
                                       .vertexStride = 12,
                                       .maxVertex = 2,
                                       .indexType = VK_INDEX_TYPE_NONE_KHR};
        inputs.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        inputs.flags = op == "blas-fast"   ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                     : op == "blas-update" ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                                           : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    else
    {
        auto instance = VkAccelerationStructureInstanceKHR{};
        instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
        instance.mask = 0xff;
        instance.accelerationStructureReference = v.last_blas;
        if (!v.buffer(sizeof(instance), input_usage, true, &instance, source))
            return false;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                       .data = {.deviceAddress = source.address}};
        inputs.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        inputs.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }

    auto start = clock_type::now();
    auto sizes = VkAccelerationStructureBuildSizesInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(v.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &inputs,
                                            &primitives, &sizes);
    report("vulkan", op + ".prebuild", seconds_since(start));
    if (op == "prebuild")
        return true;

    auto storage = vk::buffer_t{};
    auto scratch = vk::buffer_t{};
    if (!v.buffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false,
                  nullptr, storage)
        || !v.buffer(sizes.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, nullptr, scratch))
        return false;
    auto as_info = VkAccelerationStructureCreateInfoKHR{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                        .buffer = storage.buffer,
                                                        .size = sizes.accelerationStructureSize,
                                                        .type = inputs.type};
    auto as = VkAccelerationStructureKHR{};
    CHECK_VK(vkCreateAccelerationStructureKHR(v.device, &as_info, nullptr, &as));
    inputs.dstAccelerationStructure = as;
    inputs.scratchData.deviceAddress = (scratch.address + 255) & ~VkDeviceAddress(255);

    start = clock_type::now();
    CHECK_VK(vkResetCommandPool(v.device, v.pool, 0));
    auto begin = VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK_VK(vkBeginCommandBuffer(v.cmd, &begin));
    auto range = VkAccelerationStructureBuildRangeInfoKHR{.primitiveCount = primitives};
    auto const* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(v.cmd, 1, &inputs, &ranges);
    CHECK_VK(vkEndCommandBuffer(v.cmd));
    report("vulkan", op + ".record", seconds_since(start));

    start = clock_type::now();
    auto submit
        = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v.cmd};
    CHECK_VK(vkResetFences(v.device, 1, &v.fence));
    CHECK_VK(vkQueueSubmit(v.queue, 1, &submit, v.fence));
    CHECK_VK(vkWaitForFences(v.device, 1, &v.fence, VK_TRUE, UINT64_MAX));
    report("vulkan", op + ".execute", seconds_since(start));
    if (blas)
    {
        auto address = VkAccelerationStructureDeviceAddressInfoKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = as};
        v.last_blas = vkGetAccelerationStructureDeviceAddressKHR(v.device, &address);
    }
    return true;
}
#endif
} // namespace

int main(int argc, char** argv)
{
    char const* agility_dir = nullptr;
    auto list_modules = false;
    auto dx12_ops = std::vector<std::string>();
    auto vk_ops = std::vector<std::string>();
    for (auto i = 1; i < argc; ++i)
    {
        auto const flag = std::string(argv[i]);
        if (flag == "--modules")
        {
            list_modules = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            std::printf("%s needs a value\n", argv[i]);
            return 2;
        }
        auto const* value = argv[++i];
        if (flag == "--load")
        {
            if (LoadLibraryA(value) == nullptr)
            {
                std::printf("FAILED: LoadLibrary %s\n", value);
                return 2;
            }
            report("process", "load", 0.0);
        }
        else if (flag == "--agility")
            agility_dir = value;
        else if (flag == "--vendor")
            g_vendor = unsigned(std::strtoul(value, nullptr, 16));
        else if (flag == "--dx12")
            dx12_ops.emplace_back(value);
        else if (flag == "--vulkan")
            vk_ops.emplace_back(value);
        else
        {
            std::printf("unknown flag %s\n", flag.c_str());
            return 2;
        }
    }

    auto const total = clock_type::now();
    auto d = dx12{};
    auto name = std::wstring();
    if (!dx12_ops.empty() || agility_dir != nullptr)
    {
        auto const configuration_only = !dx12_ops.empty() && dx12_ops.front() == "configuration";
        auto const factory_only = !dx12_ops.empty() && dx12_ops.front() == "factory";
        if ((configuration_only || factory_only) && agility_dir == nullptr)
        {
            std::printf("FAILED: %s needs --agility\n", dx12_ops.front().c_str());
            return 2;
        }
        if (!create_dx12(d, agility_dir, configuration_only, factory_only))
            return 2;
        auto desc = DXGI_ADAPTER_DESC1{};
        d.adapter->GetDesc1(&desc);
        name = desc.Description;
        for (auto const& op : dx12_ops)
        {
            if (op == "release")
            {
                d.keep.clear();
                d.list.Reset();
                d.allocator.Reset();
                d.queue.Reset();
                d.fence.Reset();
                d.device.Reset();
                d.device_factory.Reset();
                report("dx12", "released", 0.0);
            }
            else if (op != "none" && op != "factory" && op != "configuration" && !dx12_build(d, op))
                return 2;
        }
    }
    if (!vk_ops.empty())
    {
#if REPRO_VULKAN
        auto v = vk{};
        if (!create_vk(v, name))
            return 2;
        for (auto const& op : vk_ops)
            if (op != "none" && !vk_build(v, op))
                return 2;
#else
        std::printf("FAILED: built without the Vulkan headers\n");
        return 2;
#endif
    }
    report("process", "total", seconds_since(total));
    std::printf("adapter=%ls\n", name.c_str());
    if (list_modules)
    {
        HMODULE modules[1024];
        auto needed = DWORD{};
        EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed);
        for (auto i = DWORD{}; i < needed / sizeof(HMODULE); ++i)
        {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(modules[i], path, MAX_PATH);
            auto const* base = std::strrchr(path, '\\');
            base = base ? base + 1 : path;
            if (_strnicmp(base, "ig", 2) == 0 || _strnicmp(base, "d3d12", 5) == 0)
                std::printf("  module %s\n", path);
        }
    }
    return 0;
}
