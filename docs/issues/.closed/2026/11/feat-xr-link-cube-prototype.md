---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
refs:
  - file: examples/graphics/sgl-cube
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_native_scope.hh
  - symbol: sg::backend::dx12::dx12_config
closed: 2026-11-05
resolution: fixed
---

# A cube in the headset over Quest Link, as fast as possible

A prototype before the proper `feat-xr-cube-example`: the SGL cube seen in a Quest 3 through Quest Link, on Windows, on dx12.
It takes every shortcut the proper path does not, each marked `TEMPORARY(<id>)` against the issue that replaces it.
What it proves is the loop — runtime, session, frames, poses, images — and what it learns feeds `feat-shaped-xr-core`.

## Why dx12, and not vulkan first

- The runtime names its GPU by LUID and a minimum feature level (`XR_KHR_D3D12_enable`), which `dx12_config::adapter_luid` and `minimum_feature_level` now take.
- `dx12_context::device()` and `queue()` are what `XrGraphicsBindingD3D12KHR` wants.
- `dx12_native_scope` already lets foreign code record onto an sg list, so the headset's images can be filled by a native copy before sg can wrap them.
  vulkan has no native scope yet, and its backend suite loses the device on the dev box's Arc (`bug-sg-vulkan-async-upload-submit-fails-on-arc`).
- Vulkan stays the target of the proper path; this prototype is not the architecture.

## The steps

1. **The OpenXR loader.** Fetch the Khronos loader (Apache-2.0) as an `extern/openxr` dependency, the way `extern/sdl3` is fetched.
   That is a `dependency.yml`, a fetch script, and a CMakeLists exposing an imported target.
2. **The example**, `examples/graphics/xr-cube`, starting from `sgl-cube`'s shaders and mesh.
   It talks to OpenXR directly — `TEMPORARY(feat-shaped-xr-core)` — with no library around it yet.
3. **Bring-up.** `xrCreateInstance` with `XR_KHR_D3D12_enable`, `xrGetSystem` for a head-mounted display, `xrGetD3D12GraphicsRequirementsKHR`.
   Then `sg::create_dx12_context({.adapter_luid = …, .minimum_feature_level = …})` with the LUID read as `LowPart | HighPart << 32`, and a session over `device()` and `queue()`.
4. **Images.** One `XrSwapchain` per eye, `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`, at the recommended view size.
   sg renders each eye into a texture of its own; a `dx12_native_scope` transitions the acquired XR image and copies into it.
   The runtime expects the image back in `D3D12_RESOURCE_STATE_RENDER_TARGET`.
   **Transition the XR image with legacy `ResourceBarrier` calls, never enhanced ones**, and never hand it to sg to track.
   The runtime creates it through the legacy API in RENDER_TARGET, and D3D12 lets sg's enhanced barriers meet a legacy state only at COMMON.
   So the native copy is how dx12 fills it for good, not only until `feat-sg-wrap-native-texture`: that wrap covers COMMON-state resources on dx12, and the XR image is not one.
5. **Frames.** `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews` in a `LOCAL` reference space, one draw per eye — `TEMPORARY(feat-sg-view-instancing)` — then `xrEndFrame` with one projection layer.
   The eye projection is `tg::perspective_off_center` from the view's four angles, as tangents times the near distance.
   OpenXR is right-handed with +y up and −z forward, and tg's default is left-handed with +z forward, so the pose and the view matrix flip z at the edge.
6. **Session state.** Poll `xrPollEvent` every frame and begin the session on `XR_SESSION_STATE_READY`, end it on `STOPPING`; render only while the state is `SYNCHRONIZED` or later.

## What the developer sets up once

- The Meta Quest Link app installed, signed in, and set as the active OpenXR runtime (Settings → General → OpenXR Runtime).
- The headset in developer mode, connected by Link (cable or Air Link), its proximity sensor off in Meta Quest Developer Hub so it renders unworn.
- No Vulkan SDK is needed for this path.

## Done when

- `uv run dev.py example xr-cube` shows the cube in the headset, stable as the head moves, at the runtime's refresh rate.
- Without a runtime, the example says so and exits non-zero, rather than crashing.
