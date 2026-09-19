// The cross-API form of ../repro.cc: a pending wait-before-signal in one API against device creation in the other.
// Nothing of shaped-core; D3D12, DXGI and Vulkan only.
//
//   --wait dx --create vk   a D3D12 queue waits on an unsignalled fence while another thread calls vkCreateDevice
//   --wait vk --create dx   a Vulkan queue waits on an unsignalled timeline while another thread calls D3D12CreateDevice
//   --wait dx --create none / --wait vk --create none   controls: the wait alone
//   --signal host (default) signals from the CPU; --signal queue from a second queue of the waiting API.
//
// A D3D12 device is a per-adapter singleton within a process, so `--create dx` is only a real creation when no D3D12
// device exists yet; the vk-wait cases never make one before it.
//
// Exit 0: every step finished.
// Exit 1: a step never finished within --timeout, reported before quick_exit.
// Exit 2: setup failed.
// Once a thread is stuck in the kernel the process may not be able to exit at all.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct options
{
    std::string wait = "dx";     // dx | vk
    std::string create = "vk";   // vk | dx | none
    bool host_signal = true;     // --signal queue: from a second queue of the waiting API
    int timeout_secs = 20;
    int delay_ms = 500;
};

options parse(int argc, char** argv)
{
    auto o = options();
    for (auto i = 1; i + 1 < argc; ++i)
    {
        auto const a = std::string(argv[i]);
        if (a == "--wait")
            o.wait = argv[++i];
        else if (a == "--create")
            o.create = argv[++i];
        else if (a == "--signal")
            o.host_signal = std::string(argv[++i]) == "host";
        else if (a == "--timeout")
            o.timeout_secs = std::atoi(argv[++i]);
        else if (a == "--delay")
            o.delay_ms = std::atoi(argv[++i]);
    }
    return o;
}

[[noreturn]] void fail(char const* what, long code)
{
    std::printf("%s failed: %ld\n", what, code);
    std::fflush(stdout);
    std::exit(2);
}

void check_hr(HRESULT hr, char const* what)
{
    if (FAILED(hr))
        fail(what, long(hr));
}

void check_vk(VkResult r, char const* what)
{
    if (r != VK_SUCCESS)
        fail(what, long(r));
}

std::atomic<bool> g_created = false;
auto const g_start = std::chrono::steady_clock::now();

// Milliseconds since start, so the report says whether creation finished before or after the signal.
long long ms_since_start()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
}
std::atomic<bool> g_signal_returned = false;
std::atomic<bool> g_wait_satisfied = false;

// The first hardware adapter, which is the one both APIs pick on a single-GPU box.
IDXGIAdapter1* hardware_adapter()
{
    IDXGIFactory6* factory = nullptr;
    check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    IDXGIAdapter1* adapter = nullptr;
    check_hr(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
             "EnumAdapterByGpuPreference");
    auto desc = DXGI_ADAPTER_DESC1{};
    adapter->GetDesc1(&desc);
    std::printf("dxgi adapter: %ls\n", desc.Description);
    return adapter;
}

struct vulkan_setup
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
};

vulkan_setup make_instance()
{
    auto s = vulkan_setup();
    auto const app = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    auto const info = VkInstanceCreateInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    check_vk(vkCreateInstance(&info, nullptr, &s.instance), "vkCreateInstance");
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s.instance, &count, nullptr);
    auto devices = std::vector<VkPhysicalDevice>(count);
    vkEnumeratePhysicalDevices(s.instance, &count, devices.data());
    if (count == 0)
        fail("no vulkan device", 0);
    s.gpu = devices[0];
    auto props = VkPhysicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(s.gpu, &props);
    std::printf("vulkan device: %s\n", props.deviceName);
    return s;
}

VkDevice make_vulkan_device(VkPhysicalDevice gpu, uint32_t queue_count, bool timeline)
{
    float const priorities[] = {1.0f, 1.0f};
    auto const queue = VkDeviceQueueCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = queue_count,
        .pQueuePriorities = priorities,
    };
    auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .timelineSemaphore = VK_TRUE};
    auto const info = VkDeviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = timeline ? &vk12 : nullptr,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
    };
    VkDevice device = VK_NULL_HANDLE;
    check_vk(vkCreateDevice(gpu, &info, nullptr, &device), "vkCreateDevice");
    return device;
}
} // namespace

int main(int argc, char** argv)
{
    auto const o = parse(argc, argv);
    std::printf("wait=%s create=%s signal=%s\n", o.wait.c_str(), o.create.c_str(), o.host_signal ? "host" : "queue");

    // Vulkan is needed whenever it waits or creates; D3D12 whenever it waits or creates.
    auto vk = vulkan_setup();
    if (o.wait == "vk" || o.create == "vk")
        vk = make_instance();
    IDXGIAdapter1* adapter = nullptr;
    if (o.wait == "dx" || o.create == "dx")
        adapter = hardware_adapter();

    // --- the pending wait ------------------------------------------------------------------------

    // D3D12 side
    ID3D12Device* dx_device = nullptr;
    ID3D12CommandQueue* dx_wait_queue = nullptr;
    ID3D12CommandQueue* dx_signal_queue = nullptr;
    ID3D12Fence* dx_fence = nullptr;
    ID3D12Fence* dx_drained = nullptr; // signalled by the waiting queue once its wait has passed

    // Vulkan side
    VkDevice vk_device = VK_NULL_HANDLE;
    VkQueue vk_wait_queue = VK_NULL_HANDLE;
    VkQueue vk_signal_queue = VK_NULL_HANDLE;
    VkSemaphore vk_timeline = VK_NULL_HANDLE;
    uint64_t const one = 1;

    if (o.wait == "dx")
    {
        check_hr(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dx_device)), "D3D12CreateDevice(A)");
        auto const qd = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
        check_hr(dx_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&dx_wait_queue)), "CreateCommandQueue(wait)");
        check_hr(dx_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&dx_signal_queue)), "CreateCommandQueue(signal)");
        check_hr(dx_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx_fence)), "CreateFence");
        check_hr(dx_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx_drained)), "CreateFence(drained)");
        check_hr(dx_wait_queue->Wait(dx_fence, 1), "Wait");         // nobody has signalled 1
        check_hr(dx_wait_queue->Signal(dx_drained, 1), "Signal(drained)");
    }
    else
    {
        vk_device = make_vulkan_device(vk.gpu, 2, true);
        vkGetDeviceQueue(vk_device, 0, 0, &vk_wait_queue);
        vkGetDeviceQueue(vk_device, 0, 1, &vk_signal_queue);
        auto type_info = VkSemaphoreTypeCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        auto const sem = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info};
        check_vk(vkCreateSemaphore(vk_device, &sem, nullptr, &vk_timeline), "vkCreateSemaphore");
        auto const stage = VkPipelineStageFlags(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        auto const values = VkTimelineSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                          .waitSemaphoreValueCount = 1,
                                                          .pWaitSemaphoreValues = &one};
        auto const submit = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                         .pNext = &values,
                                         .waitSemaphoreCount = 1,
                                         .pWaitSemaphores = &vk_timeline,
                                         .pWaitDstStageMask = &stage};
        check_vk(vkQueueSubmit(vk_wait_queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(wait)");
    }

    auto watchdog = std::thread(
        [&]
        {
            auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(o.timeout_secs);
            while (std::chrono::steady_clock::now() < until)
            {
                if ((o.create == "none" || g_created) && g_signal_returned && g_wait_satisfied)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::printf("HUNG after %d s: created=%d, signal returned=%d, wait satisfied=%d\n", o.timeout_secs,
                        int(g_created.load()), int(g_signal_returned.load()), int(g_wait_satisfied.load()));
            std::fflush(stdout);
            std::quick_exit(1);
        });

    // --- the other API creates a device while the wait is pending ------------------------------
    auto creator = std::thread(
        [&]
        {
            if (o.create == "vk")
            {
                auto const device = make_vulkan_device(vk.gpu, 1, false);
                g_created = true;
                std::printf("vkCreateDevice returned at %lld ms\n", ms_since_start());
                vkDestroyDevice(device, nullptr);
            }
            else if (o.create == "dx")
            {
                ID3D12Device* device = nullptr;
                check_hr(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice(B)");
                g_created = true;
                std::printf("D3D12CreateDevice returned at %lld ms\n", ms_since_start());
                device->Release();
            }
        });

    // --- release the wait ------------------------------------------------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(o.delay_ms));
    std::printf("signalling at %lld ms\n", ms_since_start());
    if (o.wait == "dx")
    {
        if (o.host_signal)
            check_hr(dx_fence->Signal(1), "ID3D12Fence::Signal");
        else
            check_hr(dx_signal_queue->Signal(dx_fence, 1), "ID3D12CommandQueue::Signal");
        g_signal_returned = true;
        while (dx_drained->GetCompletedValue() < 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        g_wait_satisfied = true;
    }
    else
    {
        if (o.host_signal)
        {
            auto const signal = VkSemaphoreSignalInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = vk_timeline, .value = one};
            check_vk(vkSignalSemaphore(vk_device, &signal), "vkSignalSemaphore");
        }
        else
        {
            auto const values = VkTimelineSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                              .signalSemaphoreValueCount = 1,
                                                              .pSignalSemaphoreValues = &one};
            auto const submit = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                             .pNext = &values,
                                             .signalSemaphoreCount = 1,
                                             .pSignalSemaphores = &vk_timeline};
            check_vk(vkQueueSubmit(vk_signal_queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(signal)");
        }
        g_signal_returned = true;
        auto const wait = VkSemaphoreWaitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &vk_timeline, .pValues = &one};
        check_vk(vkWaitSemaphores(vk_device, &wait, UINT64_MAX), "vkWaitSemaphores");
        g_wait_satisfied = true;
    }

    creator.join();
    watchdog.join();
    std::printf("ok\n");
    return 0;
}
