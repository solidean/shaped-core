---
created: 2026-11-09
priority: normal
tags: [xr]
parent: epic-xr
---

# Wrap a native GPUTexture as an sg texture on webgpu

A `GPUTexture` from WebXR's projection layer, as an sg texture: `feat-xr-webxr` needs it.
vulkan, dx12 and metal have theirs (`wrap_vulkan_texture`, `wrap_dx12_texture`, `wrap_metal_texture`), each with a render-and-read-back test and a refusal of a mismatched shape.
`backends/metal/tests/metal-wrap-texture-test.cc` is the closest model: a texture handed over by another framework, which sg keeps alive while recorded work uses it.
Split from the metal half because it can only be tested in a browser with WebGPU.
