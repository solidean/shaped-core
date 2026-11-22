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

## It lands with a GLSL target

The port's case is a raw OpenGL ES pass outside wgpu, so what it needs from SGL is GLSL ES text with `samplerExternalOES` and `GL_OES_EGL_image_external_essl3`.
SGL has no GLSL target yet; the spec lists it as open (emitting.md, "GLSL, which comes through the same seam"), and sg's opengl and webgl tiers have no backend.
So the GLSL ES form of this type waits on that target, and the two are best built together; the WGSL form, `texture_external`, need not wait.
