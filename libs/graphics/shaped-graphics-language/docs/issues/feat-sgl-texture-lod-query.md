---
created: 2026-11-22
priority: next
tags: [porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/anisotropic-lod.md
---

# A pixel stage asks a sampled texture which mip level it would read

There is no level-of-detail query: `texture_2d[float4]` has no member `lod`.
Ports approximate it from derivatives today.

- Native on dx12 (`CalculateLevelOfDetail`, `CalculateLevelOfDetailUnclamped`), vulkan (`OpImageQueryLod`) and metal (`calculate_clamped_lod`, `calculate_unclamped_lod`).
- WebGPU has none, so it is a polyfill from `dpdx` and `dpdy` and the texture's size, reported as emulated the way ray queries are.
- It takes derivatives implicitly, so it stands under CHK-282's uniformity rule as `sample` does.
- **The polyfill cannot promise the hardware's number.**
  Vendors already disagree on anisotropic levels ([anisotropic-lod](../../../shaped-graphics/docs/concepts/anisotropic-lod.md)), so its test states a tolerance rather than equality.
- Open: clamped, unclamped or both, and whether the sampler's own bias and clamps count, which the targets answer differently.
