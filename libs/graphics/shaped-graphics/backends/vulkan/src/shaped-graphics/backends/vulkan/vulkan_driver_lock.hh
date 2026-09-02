#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>

#include <mutex> // std::unique_lock, which <shared_mutex> does not itself guarantee
#include <shared_mutex>

// A process-global reader/writer lock over two Vulkan call families that must not overlap.
//
// This exists for a driver bug, not for anything sg's own design requires.
// On the NVIDIA proprietary driver (591.86, Windows), a thread inside vkCreateRayTracingPipelinesKHR and a thread
// inside vkCreateDevice / vkDestroyDevice deadlock against each other: one holds a driver-internal lock and wants the
// loader's, the other has the loader's and wants the driver's.
// docs/bugs-external/vulkan-concurrent-device-lifecycle-deadlock reproduces it in raw Vulkan with nothing of ours in
// the picture, and the same binary against an AMD adapter does not hang.
//
// The pairing is what the repro established, and it is why this is a shared lock rather than a mutex:
//   - concurrent device create/destroy alone is fine,
//   - concurrent ray-tracing pipeline builds alone are fine,
//   - the two together hang.
// So pipeline builds are readers and device lifecycle is the writer.
// Several ray-tracing pipelines still compile in parallel, which is the case that actually costs wall-clock; what is
// serialized against them is device creation, which happens a handful of times in a process.
//
// Process-global rather than per-context, because the driver state it protects is process-wide: two contexts on two
// devices deadlock exactly as one does.
//
// `std::shared_mutex` because clean-core has no shared/exclusive lock yet — the same gap
// libs/graphics/shaped-graphics/docs/TODO.md records for render routines, which want `cc::shared_mutex<T>` next to
// `cc::mutex<T>`. Move this to it when that lands.

namespace sg::backend::vulkan
{
struct scoped_raytracing_build;
struct scoped_device_lifecycle;

/// The lock itself.
/// Prefer the two guards below to taking it directly.
[[nodiscard]] std::shared_mutex& driver_lifecycle_lock();
} // namespace sg::backend::vulkan

/// Held across a ray-tracing pipeline build.
/// Shared: builds run in parallel with each other, and only exclude device creation and teardown.
struct sg::backend::vulkan::scoped_raytracing_build
{
    scoped_raytracing_build() : _guard(driver_lifecycle_lock()) {}

    scoped_raytracing_build(scoped_raytracing_build const&) = delete;
    scoped_raytracing_build& operator=(scoped_raytracing_build const&) = delete;

private:
    std::shared_lock<std::shared_mutex> _guard;
};

/// Held across instance/device creation and teardown.
/// Exclusive: nothing else in the process may be inside a ray-tracing build while this runs.
struct sg::backend::vulkan::scoped_device_lifecycle
{
    scoped_device_lifecycle() : _guard(driver_lifecycle_lock()) {}

    scoped_device_lifecycle(scoped_device_lifecycle const&) = delete;
    scoped_device_lifecycle& operator=(scoped_device_lifecycle const&) = delete;

private:
    std::unique_lock<std::shared_mutex> _guard;
};
