---
created: 2026-11-22
priority: someday
tags: [porting-feedback, webgpu]
---

# A texture a video decoder writes, sampled as the platform's external texture

A port's headset client samples the hardware video decoder's frames, which GLSL ES reads as a `samplerExternalOES`.
SGL has no form for it, so that one pass stayed hand-written GLSL.

The targets' forms differ:

- WGSL's `texture_external`, sampled with `textureSampleBaseClampToEdge`, which WebGPU fills from a video frame.
- GLSL ES's `samplerExternalOES`, which naga's GLSL writer would have to take from WGSL.
- vulkan's sampler Y'CbCr conversion, an immutable sampler that converts as it samples.
- dx12 and metal give a decoder's planes as ordinary textures, which the shader converts itself.

Whether one SGL type can stand for all of them, or WGSL's alone is worth carrying, is the first question.
