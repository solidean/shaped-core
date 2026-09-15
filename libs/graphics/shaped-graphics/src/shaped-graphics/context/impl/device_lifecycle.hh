#pragma once

#include <shaped-graphics/fwd.hh>

// A process-global reader/writer lock between device lifecycle and ray-tracing driver calls, across every backend.
//
// This exists for a driver bug, not for anything sg's own design requires.
// On the NVIDIA proprietary driver (Windows), device creation or teardown on one thread deadlocks inside the driver
// against ray-tracing work on another, each holding a driver-internal lock the other wants.
// It does not care which API either side came through:
//   - vkCreateDevice / vkDestroyDevice against vkCreateRayTracingPipelinesKHR, which
//     docs/bugs-external/vulkan-concurrent-device-lifecycle-deadlock reproduces in raw Vulkan;
//   - vkDestroyDevice against a D3D12 context releasing its resources, and against a D3D12 BLAS build,
//     both seen as hangs in shaped-graphics-test when its vulkan and dx12 entry drivers ran side by side.
//
// Device creation and teardown take it exclusively, so they are serialized against each other and against ray tracing.
// A process creates a handful of devices, so that costs nothing that matters.
// Ray-tracing driver calls take it shared, so they still run in parallel with each other, which is the case that costs wall clock.
//
// Process-global rather than per-context, because the driver state it protects is process-wide.

namespace sg::impl
{
class device_lifecycle_hold;
class raytracing_driver_hold;
} // namespace sg::impl

/// Holds the lock exclusively for as long as this object lives: device creation and teardown.
/// Reentrant on the thread that already holds it, since a failed creation destroys its half-built context while holding
/// it, and a context's destructor calls shutdown().
/// Must not be taken inside a raytracing_driver_hold on the same thread.
class sg::impl::device_lifecycle_hold
{
public:
    device_lifecycle_hold();
    ~device_lifecycle_hold();

    device_lifecycle_hold(device_lifecycle_hold const&) = delete;
    device_lifecycle_hold& operator=(device_lifecycle_hold const&) = delete;

private:
    bool _owns = false;
};

/// Holds the lock shared for as long as this object lives: a ray-tracing driver call.
/// A no-op on a thread that already holds the lock either way.
class sg::impl::raytracing_driver_hold
{
public:
    raytracing_driver_hold();
    ~raytracing_driver_hold();

    raytracing_driver_hold(raytracing_driver_hold const&) = delete;
    raytracing_driver_hold& operator=(raytracing_driver_hold const&) = delete;

private:
    bool _counted = false;
    bool _owns = false;
};
