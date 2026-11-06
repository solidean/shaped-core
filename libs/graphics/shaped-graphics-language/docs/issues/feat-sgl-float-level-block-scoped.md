---
created: 2026-11-06
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# `@float` on a block

A level is stated per function and kept per entry point, because that is the granularity DXC's `-Gis` and metal's safe math have.
A shader wants `ieee` around the few lines that compute an error term, and `fast` everywhere else, **with no cost outside those lines**.

**That is what this issue is, and it is blocked on [epic-sgl-native-ir-targets](epic-sgl-native-ir-targets.md).**
Each operation has to carry its own guarantee, through `NoContraction` and `FPFastMathMode` in SPIR-V and fast-math flags in DXIL.
Raising the whole entry point to a block's level would be conforming, since a level is a license rather than an obligation.
It is not this feature: its whole-entry cost is what the feature exists to avoid.

Denormal handling stays per entry point in SPIR-V and DXIL, so `ieee` inside a block still reaches beyond the block there.
Metal and WebGPU have no per-operation form, so what a block means on those is part of the design.

To settle: the spelling on a block, and whether a block may loosen what its function states, or only tighten it.
