---
created: 2026-11-22
priority: next
tags: [porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/uniformity.md
closed: 2026-11-22
resolution: fixed
---

# A branch on quad_any keeps a derivative in uniform control flow

A port wants an expensive gradient computation only in the pixels that need it, under `if subgroup_any(cond):`.
SGL refuses that, and rightly: `subgroup_any` is uniform across the active lanes of a subgroup, and a pixel stage's helper lanes need not be active in a subgroup operation on vulkan and WebGPU.
So a helper lane may take the other side of the branch, and the derivative of the quad it belongs to is undefined.

Wanted: `quad_any(b)` and `quad_all(b)`, whose result the uniformity pass reads as uniform across the quad, helpers included.

- dx12 `QuadAny` and `QuadAll` (Shader Model 6.7), or `QuadReadAcrossX/Y/Diagonal` below it; metal `quad_any` and `quad_all`.
- vulkan `OpGroupNonUniformQuadAnyKHR` or the quad swaps; WebGPU `quadSwapX`, `quadSwapY` and `quadSwapDiagonal` combined.
- **Whether helper lanes take part in each target's quad operations is what makes this sound**, and is to be read from each spec before the pass trusts the result.
- `non-uniform-control-flow`'s note on a branch over `subgroup_any` suggests `quad_any`.
- `subgroup_any` stays non-uniform for derivatives.
