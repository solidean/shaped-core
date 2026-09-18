// vkCreateDevice against a pending wait-before-signal on another device, with nothing of shaped-core.
//
// Device A queues a submission that waits on a timeline value nobody has signalled yet — legal Vulkan, and what an
// engine does when a later queue must wait on a value a transfer thread signals afterwards.
// A second thread then creates device B, and after a delay the main thread signals the value.
// On the affected driver vkCreateDevice waits for the GPU to go idle while holding a lock that every way of signalling
// needs — a queue submit and a host vkSignalSemaphore alike — so none of them ever returns.
//
// Exit 0: every step finished.
// Exit 1: a step never finished within --timeout, reported before quick_exit.
// Exit 2: no usable device.
// Once a thread is stuck in the kernel the process may not be able to exit at all, which run.py reports as HUNG(hard).

#include <vulkan/vulkan.h>

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
    int device = 0;
    bool validation = false;
    int timeout_secs = 30;
    int delay_ms = 500;
    bool pending_wait = true;      // --no-wait: device A queues nothing
    bool concurrent_create = true; // --no-create: nobody creates device B
    bool host_signal = false;      // --signal host: vkSignalSemaphore instead of a queue submit
};

options parse(int argc, char** argv)
{
    auto o = options();
    for (auto i = 1; i < argc; ++i)
    {
        auto const a = std::string(argv[i]);
        auto next = [&] { return i + 1 < argc ? std::atoi(argv[++i]) : 0; };
        if (a == "--device")
            o.device = next();
        else if (a == "--timeout")
            o.timeout_secs = next();
        else if (a == "--delay")
            o.delay_ms = next();
        else if (a == "--validation")
            o.validation = true;
        else if (a == "--no-wait")
            o.pending_wait = false;
        else if (a == "--no-create")
            o.concurrent_create = false;
        else if (a == "--signal" && i + 1 < argc)
            o.host_signal = std::string(argv[++i]) == "host";
    }
    return o;
}

void check(VkResult r, char const* what)
{
    if (r != VK_SUCCESS)
    {
        std::printf("%s failed: %d\n", what, int(r));
        std::fflush(stdout);
        std::exit(2);
    }
}

// Progress, read by the watchdog to say which step never finished.
std::atomic<bool> g_device_b_created = false;
std::atomic<bool> g_signal_returned = false;
std::atomic<bool> g_wait_satisfied = false;
} // namespace

int main(int argc, char** argv)
{
    auto const o = parse(argc, argv);

    auto const app = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    char const* layers[] = {"VK_LAYER_KHRONOS_validation"};
    auto const instance_info = VkInstanceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledLayerCount = o.validation ? 1u : 0u,
        .ppEnabledLayerNames = layers,
    };
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    auto physical = std::vector<VkPhysicalDevice>(count);
    vkEnumeratePhysicalDevices(instance, &count, physical.data());
    if (o.device < 0 || uint32_t(o.device) >= count)
    {
        std::printf("no device %d (have %u)\n", o.device, count);
        return 2;
    }
    auto const gpu = physical[o.device];

    auto props = VkPhysicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    std::printf("device %d: %s (driver 0x%x)\n", o.device, props.deviceName, props.driverVersion);

    // Two queues: one waits, the other signals, as an engine's direct and copy queues do.
    // A signal behind the wait on ONE queue could never run at all, since a queue executes in order.
    // Family 0 is universal on every desktop driver that matters here; the second queue may come from another family.
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
    auto families = std::vector<VkQueueFamilyProperties>(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
    auto signal_family = 0u;
    if (families[0].queueCount < 2)
        for (auto f = 1u; f < family_count; ++f)
            if (families[f].queueCount > 0)
            {
                signal_family = f;
                break;
            }
    float const priorities[] = {1.0f, 1.0f};
    auto queue_infos = std::vector<VkDeviceQueueCreateInfo>();
    queue_infos.push_back({
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = signal_family == 0 ? 2u : 1u,
        .pQueuePriorities = priorities,
    });
    if (signal_family != 0)
        queue_infos.push_back({
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = signal_family,
            .queueCount = 1,
            .pQueuePriorities = priorities,
        });
    auto const queue_info = queue_infos[0]; // device B needs only the first
    auto vk12 = VkPhysicalDeviceVulkan12Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .timelineSemaphore = VK_TRUE};
    auto const device_info = VkDeviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &vk12,
        .queueCreateInfoCount = uint32_t(queue_infos.size()),
        .pQueueCreateInfos = queue_infos.data(),
    };

    VkDevice device_a = VK_NULL_HANDLE;
    check(vkCreateDevice(gpu, &device_info, nullptr, &device_a), "vkCreateDevice(A)");
    VkQueue queue_a = VK_NULL_HANDLE;
    VkQueue signal_queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device_a, 0, 0, &queue_a);
    vkGetDeviceQueue(device_a, signal_family, signal_family == 0 ? 1 : 0, &signal_queue);

    auto type_info = VkSemaphoreTypeCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                               .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                               .initialValue = 0};
    auto const sem_info = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info};
    VkSemaphore timeline = VK_NULL_HANDLE;
    check(vkCreateSemaphore(device_a, &sem_info, nullptr, &timeline), "vkCreateSemaphore");

    uint64_t const one = 1;

    // The pending wait: a submission on A that cannot start until `timeline` reaches 1, which nothing has signalled.
    if (o.pending_wait)
    {
        auto const stage = VkPipelineStageFlags(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        auto const wait_values = VkTimelineSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &one,
        };
        auto const wait_submit = VkSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &wait_values,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &timeline,
            .pWaitDstStageMask = &stage,
        };
        check(vkQueueSubmit(queue_a, 1, &wait_submit, VK_NULL_HANDLE), "vkQueueSubmit(wait)");
    }

    // Reports which step never finished, then leaves without joining: a joined thread stuck in the driver would hang
    // the report too.
    auto watchdog = std::thread(
        [&]
        {
            auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(o.timeout_secs);
            while (std::chrono::steady_clock::now() < until)
            {
                if ((!o.concurrent_create || g_device_b_created) && g_signal_returned && g_wait_satisfied)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::printf("HUNG after %d s: device B created=%d, signal returned=%d, wait satisfied=%d\n", o.timeout_secs,
                        int(g_device_b_created.load()), int(g_signal_returned.load()), int(g_wait_satisfied.load()));
            std::fflush(stdout);
            std::quick_exit(1);
        });

    // Device B, created while A's wait is pending.
    auto creator = std::thread(
        [&]
        {
            if (!o.concurrent_create)
                return;
            auto single = queue_info;
            single.queueCount = 1;
            auto const plain = VkDeviceCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .queueCreateInfoCount = 1,
                .pQueueCreateInfos = &single,
            };
            VkDevice device_b = VK_NULL_HANDLE;
            check(vkCreateDevice(gpu, &plain, nullptr, &device_b), "vkCreateDevice(B)");
            g_device_b_created = true;
            vkDestroyDevice(device_b, nullptr);
        });

    // Give B's creation time to be inside the driver, then release A's wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(o.delay_ms));
    if (o.host_signal)
    {
        auto const signal = VkSemaphoreSignalInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = timeline, .value = one};
        check(vkSignalSemaphore(device_a, &signal), "vkSignalSemaphore");
    }
    else
    {
        auto const signal_values = VkTimelineSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &one,
        };
        auto const signal_submit = VkSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &signal_values,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &timeline,
        };
        check(vkQueueSubmit(signal_queue, 1, &signal_submit, VK_NULL_HANDLE), "vkQueueSubmit(signal)");
    }
    g_signal_returned = true;

    auto const wait_info = VkSemaphoreWaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &timeline, .pValues = &one};
    check(vkWaitSemaphores(device_a, &wait_info, UINT64_MAX), "vkWaitSemaphores");
    g_wait_satisfied = true;

    creator.join();
    watchdog.join();

    vkDeviceWaitIdle(device_a);
    vkDestroySemaphore(device_a, timeline, nullptr);
    vkDestroyDevice(device_a, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::printf("ok\n");
    return 0;
}
