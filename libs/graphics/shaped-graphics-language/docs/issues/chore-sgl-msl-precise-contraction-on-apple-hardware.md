---
created: 2026-11-26
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
---

# metal: check on Apple hardware that a `.precise` scope's MSL keeps every multiply and add apart

EMIT-281 promises that in a `.precise` or `.ieee` scope every `+`, `-`, `*` and `/` of floats is one IEEE operation,
in source order, none fused into another, on every route.
The MSL text states `math_mode(safe)` at those levels (EMIT-185) and writes products and `smoothstep` through the
`sgl_ordered_*` helpers, but it writes no `#pragma METAL fp contract(off)`.
Whether Metal's safe math alone stops the compiler from contracting a written `a * b + c` into a fused multiply-add is
not established: the text was only checked for its content when EMIT-281 landed (545c64dee), never compiled or run on
an Apple GPU.

What a run should establish, on an M-series GPU:

- compile a `.precise` entry with a written `a * b + c` and with the `sgl_ordered_*` helpers, and read the AIR or the
  GPU binary for fused multiply-adds (`fma`, `llvm.fmuladd`);
- run a probe whose result differs by an ULP between the fused and the unfused form (a product near a power of two
  plus its negated rounded value) and compare with the interpreter's bits;
- if contraction happens, add `#pragma METAL fp contract(off)` (or the per-function form) to the text at `.precise`
  and `.ieee`, with an emit test, and check it again.

Until then EMIT-281's guarantee is unverified on Metal. Found while landing EMIT-281 for Quest3D's SGL migration
(D103 of its sgl-migration-readiness plan); nobody there has an Apple device.
