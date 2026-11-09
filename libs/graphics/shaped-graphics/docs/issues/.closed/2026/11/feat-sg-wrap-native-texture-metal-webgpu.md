---
created: 2026-11-05
priority: normal
tags: [xr]
parent: epic-xr
closed: 2026-11-09
resolution: fixed
---

# Wrap a native image as an sg texture on metal and webgpu

The vulkan and dx12 halves exist (`wrap_vulkan_texture`, `wrap_dx12_texture`); this is the same for the other two backends.

- **webgpu**: a `GPUTexture` from WebXR's projection layer, which `feat-xr-webxr` needs.
- **metal**: an `MTLTexture` from Compositor Services, for visionOS later.

Neither was done with the others because neither could be tested on the machine they were written on.

## Resolution

metal: `metal_context::wrap_metal_texture(MTL::Texture*, desc)` retains the texture, adds it to the residency set, and releases it deferred once no recorded work uses it.
It refuses a shape other than the description's, a format other than the texture's own unless it allows pixel-format views, and a usage the description needs that the texture lacks.
`metal-wrap-texture-test.cc` renders into a wrapped texture whose owner released it at once, reads it back, and pins both refusals.
webgpu moved to `feat-sg-wrap-native-texture-webgpu`.
