---
created: 2026-11-05
priority: normal
tags: [xr]
parent: epic-xr
---

# Wrap a native image as an sg texture on metal and webgpu

The vulkan and dx12 halves exist (`wrap_vulkan_texture`, `wrap_dx12_texture`); this is the same for the other two backends.

- **webgpu**: a `GPUTexture` from WebXR's projection layer, which `feat-xr-webxr` needs.
- **metal**: an `MTLTexture` from Compositor Services, for visionOS later.

Neither was done with the others because neither could be tested on the machine they were written on.
