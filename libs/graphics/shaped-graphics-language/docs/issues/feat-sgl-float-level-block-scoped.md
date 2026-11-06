---
created: 2026-11-06
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# `@float` on a block

A level is stated per function and kept per entry point, because that is the granularity DXC's `-Gis` and metal's safe math have.
A shader wants `ieee` around the few lines that compute an error term, and `fast` everywhere else.

**The rest of the entry point has to stay fast, and that is what waits for a native target.**
A level is a license a target may take rather than an obligation to be fast, so keeping more than a block asks is conforming, and the semantics alone are cheap.
A text target could only raise the whole entry point to the strictest level in it, which is the cost this feature exists to remove.
So this is blocked on [epic-sgl-native-ir-targets](epic-sgl-native-ir-targets.md), where each operation carries its own guarantee already.
That goes out as `NoContraction` and `FPFastMathMode` in SPIR-V, and fast-math flags in DXIL.
Denormal handling stays per entry point on every target, so `ieee` inside a block is what still raises an entry point there.

To settle: the spelling on a block, and whether a block may loosen what its function states, or only tighten it.
