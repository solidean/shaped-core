---
created: 2026-10-30
corpus:
  - bgfx/shaders/52-layered/vs_layered
---

# A vertex output that sends a primitive to a render-target array layer

bgfx's layered example writes `gl_Layer` / `SV_RenderTargetArrayIndex` from the vertex stage, to draw both eyes of a stereo frame in one pass.
SGL's stage-edge values (CHK-271) have no such output, so the translation passes the layer as a flat varying, marked `// gap:`.

- **Likely shape**: a member attribute on the vertex output struct, `@layer layer: int`, beside `@position`.
- **Target support differs**: dx12 and vulkan need a device feature for it from the vertex stage (VPAndRTArrayIndexFromAnyShader, shaderOutputLayer).
  Metal has `[[render_target_array_index]]`, and core WebGPU has none, so it is a feature a device grants, as `epic-sgl-feature-grants` has them.
