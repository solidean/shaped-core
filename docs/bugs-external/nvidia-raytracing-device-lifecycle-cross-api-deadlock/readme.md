# NVIDIA: ray-tracing work deadlocks against ray-tracing Vulkan device lifecycle, across Vulkan and D3D12

**Status:** open, not yet filed upstream; worked around in shaped-graphics.
**Affects:** NVIDIA proprietary driver 591.86 on Windows 11, RTX 5070 Ti.
**Found by:** `shaped-graphics-test` hanging past its 60 s timeout, once in a few runs, after its vulkan and dx12 entry drivers began overlapping.

This is the cross-API form of [vulkan-concurrent-device-lifecycle-deadlock](../vulkan-concurrent-device-lifecycle-deadlock/readme.md).
That entry isolated the Vulkan-only form on an RTX 4090; this one shows the other half can be D3D12.

## What happens

The hang dumps from the test binary, reduced to our frames and the driver's:

```raw
thread A  nvcuda64.dll  <- VkLayer_khronos_validation  <- vkDestroyDevice  <- sg::backend::vulkan::vulkan_context::shutdown
thread B  nvcuda64.dll  <- OpenAdapter10  <- D3D12Core  <- dx12_memory_heap released in sg::context::~context

thread A  nvcuda64.dll  <- vkDestroyDevice  <- vulkan_context::shutdown
thread B  nvcuda64.dll  <- D3D12Core  <- ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo  <- dx12_command_list::build_blas_common
```

Both threads wait on locks inside `nvcuda64.dll`, the driver component the Vulkan ICD and the D3D12 UMD both call into.
Neither ever returns.

## The trigger, isolated

`repro.cc` runs two threads, each looping over one role; `run.py` runs every pair below.
No validation layer and no D3D12 debug layer are enabled, and one thread per side is enough.

| role | per round |
|---|---|
| `vk` | create + destroy a `VkInstance` and a `VkDevice` with the ray-tracing extensions enabled |
| `vk-plain` | the same, without the ray-tracing extensions |
| `vk-sizes` | `vk`, plus `vkGetAccelerationStructureBuildSizesKHR` before destroying the device |
| `vk-pipeline` | `vk`, plus one ray-tracing pipeline built before destroying the device |
| `dx` | create + release an `ID3D12Device` |
| `dx-prebuild` | `dx`, plus `GetRaytracingAccelerationStructurePrebuildInfo` before releasing it |

```
pair                        hangs  attempts
--------------------------  -----  --------
vk vs dx-prebuild           2/2    HUNG, HUNG
dx-prebuild vs vk-pipeline  2/2    HUNG, HUNG
vk vs vk-pipeline           2/2    HUNG, HUNG
vk-plain vs dx-prebuild     0/2    ok, ok
vk-plain vs vk-pipeline     0/2    ok, ok
dx vs vk-pipeline           0/2    ok, ok
vk-sizes vs dx              0/2    ok, ok
vk vs vk-sizes              0/2    ok, ok
dx vs dx-prebuild           0/2    ok, ok
dx-prebuild vs dx-prebuild  0/2    ok, ok
```

Every hang has the same two halves:
- one thread **creates or destroys a Vulkan device with the ray-tracing extensions enabled**;
- the other makes the **first ray-tracing call on a freshly created device**, in either API: `vkCreateRayTracingPipelinesKHR`, or D3D12's prebuild query.

Remove either half and it runs.
A Vulkan device without the extensions never hangs, and neither does a D3D12 device that is not a fresh one.
`vkGetAccelerationStructureBuildSizesKHR` is not enough to trigger it.

D3D12 against D3D12 did not hang, with one caveat.
D3D12 devices are singletons per adapter, so two D3D12 threads mostly share one live device and never tear it down independently.
A D3D12-only form is not ruled out, only not reachable the way the Vulkan one is.

## Why it is the driver and not us

The usage is legal in both APIs.

The Vulkan specification's [threading behavior](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#fundamentals-threadingbehavior) section is the rule.
"All commands support being called concurrently from multiple threads", except for parameters marked externally synchronized.
`vkCreateDevice` marks none, `vkDestroyDevice` marks only its own `device` and that device's queues, and `vkCreateRayTracingPipelinesKHR` marks only its `pipelineCache`.
Building a pipeline on one device while another thread destroys a different device shares no externally synchronized object.

D3D12 resource creation [is free-threaded](https://learn.microsoft.com/en-us/windows/win32/direct3d12/uploading-resources).
Its only threading restriction is per [command list](https://learn.microsoft.com/en-us/windows/win32/direct3d12/design-philosophy-of-command-queues-and-command-lists).
Nothing in either API asks an application to synchronize across APIs.

`repro.cc` includes nothing of ours, and every call in it is a plain create, query or destroy.

## Is it known

Not that we could find.
The 591.86 release notes and the NVIDIA Vulkan beta changelog through 596.83 list no such hang, and neither the Khronos trackers nor engine trackers have this trigger.

The same class of bug has been reported before, which fits a lock-order inversion inside NVIDIA's shared driver components:
- [NVIDIA forum, 2016][nvidia-forum-2016]: concurrent `vkCreateDevice` / `vkDestroyDevice` deadlocks.
  NVIDIA fixed it, and it later regressed once.
- [wgpu discussion #9092, 2026](https://github.com/gfx-rs/wgpu/discussions/9092): Vulkan and EGL torn down concurrently deadlock inside the NVIDIA Linux driver, whose backends share mutexes.
- [NVIDIA forum, 2025](https://forums.developer.nvidia.com/t/vkdestroydevice-hang-on-linux-libnvidia-glcore-so-535-216-01/319139): a lock cycle inside `vkDestroyDevice` on Linux.
- [Godot #119125](https://github.com/godotengine/godot/issues/119125): an unrelated crash whose stack shows `vkCreateDevice` on Windows entering `nvcuda64.dll`, the component both our threads wait in.

It is worth filing: the repro is small, standalone, and nothing public matches it.

[nvidia-forum-2016]: https://forums.developer.nvidia.com/t/creating-destroying-several-vkdevices-concurrently-sometimes-crashes-or-deadlocks/43036

## Reproducing

```bash
uv run run.py               # the full matrix
uv run run.py --quick       # just vk vs dx-prebuild
uv run run.py --repeat 5    # more attempts per pair
```

Exit code 1 means it reproduced.
The script finds the Vulkan SDK through `VULKAN_SDK` and builds with `clang-cl`, which finds the Windows SDK itself; there is no shaped-core dependency and no `dev.py`.
The repro terminates itself once no round finishes for five seconds, since joining threads stuck in the driver would hang the report too.

## The workaround in shaped-core

[device_lifecycle.hh](../../../libs/graphics/shaped-graphics/src/shaped-graphics/context/impl/device_lifecycle.hh) holds one process-global reader/writer lock for both backends.
- `sg::impl::device_lifecycle_hold` takes it **exclusive** across context creation and the whole of context teardown, in vulkan and dx12 alike.
- `sg::impl::raytracing_driver_hold` takes it **shared** around `vkCreateRayTracingPipelinesKHR` and the D3D12 prebuild, build and state-object calls.

That is broader than the matrix strictly needs, since D3D12 device lifecycle alone never took part and neither did a plain Vulkan device.
A process creates a handful of devices, so the exclusive side costs nothing measurable, and one rule for both backends is simpler to keep correct than a precise one.

## What to check when a new NVIDIA driver lands

Run `run.py`.
If every pair reports `ok` across several attempts, the driver is fixed for this form.
The lock serves both this and [vulkan-concurrent-device-lifecycle-deadlock](../vulkan-concurrent-device-lifecycle-deadlock/readme.md).
Retire `device_lifecycle.hh` together with this directory only once that one is fixed as well.
