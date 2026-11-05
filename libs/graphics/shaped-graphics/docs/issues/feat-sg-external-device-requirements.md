---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
refs:
  - symbol: sg::backend::vulkan::vulkan_config
  - symbol: sg::backend::dx12::dx12_config
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-05T12:44Z
---

# Contexts created to a spec someone else dictates

An XR runtime chooses the GPU and the device features, so sg's backends must accept those requirements instead of choosing on their own.

## Wanted

- `vulkan_config`: extra instance and device extensions, a physical device to use, and a hook that creates the instance and device.
  OpenXR's `XR_KHR_vulkan_enable2` calls `vkCreateDevice` itself.
- `dx12_config`: an adapter by LUID and a minimum feature level.
- A context exports its native device, queue and queue family to whoever created it.
- webgpu: adopt a `GPUDevice` the page already has, for WebXR.

## Known

- The requirements are the primitive and shaped-xr creating the context is convenience on top (decided in `epic-xr`).
- webgpu needs nothing new: `sg::create_webgpu_context(WGPUDevice, …)` already adopts a device someone else requested.

## Progress (2026-11-05)

- **dx12 is done and tested**: `dx12_config::adapter_luid` and `minimum_feature_level`, `dx12_context::device()` / `queue()`.
- **`sg::adapter_info::luid`** is reported by dx12 and by vulkan, so a context on one API can name its GPU to the other, or to an XR runtime.
- **vulkan is written and not yet compiled**: the development machine has no Vulkan SDK, so configure skips the backend.
  `vulkan_config` gained the extra extensions and the `create_instance` / `choose_physical_device` / `create_device` hooks, and `vulkan_context` the native accessors.
  `vulkan-external-device-test.cc` covers them; it runs once the SDK is installed.
