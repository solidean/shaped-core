---
created: 2026-11-06
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# `@float` on a block

A level is stated per function and kept per entry point, because that is the granularity DXC's `-Gis` and metal's safe math have.
A shader wants `ieee` around the few lines that compute an error term, and `fast` everywhere else.

**This does not wait for a native target.**
A level is a license a target may take rather than an obligation to be fast, so keeping more than a block asks is always conforming.
So a text target keeps a block's level by raising the whole entry point to the strictest level in it, which costs speed and never correctness.
`precise` in HLSL is per variable, so dx12 and vulkan can do better than that for the `precise` level already.

**What a native target adds** is the cost: each operation carries its own guarantee, through `NoContraction` and `FPFastMathMode` in SPIR-V and fast-math flags in DXIL.
Denormal handling stays per entry point on every target, so `ieee` inside a block is what still raises an entry point there.
See [epic-sgl-native-ir-targets](epic-sgl-native-ir-targets.md).

To settle: the spelling on a block, and whether a block may loosen what its function states, or only tighten it.
