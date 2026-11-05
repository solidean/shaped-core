---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
closed: 2026-11-05
resolution: fixed
---

# Wrap a native image as an sg texture and render target

XR runtimes hand out their own swapchain images, and sg can only use textures it created.

## Wanted

- A per-backend way to wrap a `VkImage`, an `ID3D12Resource`, an `MTLTexture` or a `GPUTexture` as an array texture usable as a render target.
- The image's state on entry and on exit, so barrier tracking leaves it where the runtime expects on release: colour attachment on vulkan, render target on dx12.
- The wrapper does not own the image; the runtime destroys it.

## Known

- Headset images are wrapped textures, not an `sg::swapchain`, whose one-present-per-acquire contract does not fit XR's wait / begin / end.

## Done (2026-11-05)

- **vulkan**: `vulkan_context::wrap_vulkan_texture(image, desc, layout)` tracks the image from the layout it is in, and `cmd.ensure_layout` hands it back.
  This is the XR path: an OpenXR Vulkan image is handed out at colour attachment and wanted back there.
- **dx12 can wrap only a resource that is in COMMON at every submit.**
  sg barriers with enhanced barriers, which D3D12 lets meet a legacy resource state only at COMMON, and the debug layer refuses anything else at the first submit.
  An OpenXR D3D12 image is a legacy resource handed out in RENDER_TARGET, so on dx12 it is filled from a `dx12_native_scope` with legacy barriers.
  `wrap_dx12_texture` serves the resources that rest in COMMON.
- **The exit state needed no new API**: `cmd.ensure_layout` already leaves a texture in a named layout.
- metal and webgpu are `feat-sg-wrap-native-texture-metal-webgpu`.
