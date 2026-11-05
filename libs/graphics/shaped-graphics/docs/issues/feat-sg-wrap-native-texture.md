---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-05T13:59Z
---

# Wrap a native image as an sg texture and render target

XR runtimes hand out their own swapchain images, and sg can only use textures it created.

## Wanted

- A per-backend way to wrap a `VkImage`, an `ID3D12Resource`, an `MTLTexture` or a `GPUTexture` as an array texture usable as a render target.
- The image's state on entry and on exit, so barrier tracking leaves it where the runtime expects on release: colour attachment on vulkan, render target on dx12.
- The wrapper does not own the image; the runtime destroys it.

## Known

- Headset images are wrapped textures, not an `sg::swapchain`, whose one-present-per-acquire contract does not fit XR's wait / begin / end.
