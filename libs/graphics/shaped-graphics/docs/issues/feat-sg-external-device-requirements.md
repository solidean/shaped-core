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
