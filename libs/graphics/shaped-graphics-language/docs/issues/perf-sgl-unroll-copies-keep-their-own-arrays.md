---
created: 2026-11-22
tags: [porting-feedback, dx12]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/text_writer.cc
---

# Each @unroll copy of a loop declares its own local arrays, where DXC's [unroll] reuses one

A BC7-style kernel with per-iteration `float[3]` arrays, unrolled, keeps 30 `alloca [3 x float]` on SGL's HLSL route against 19 for the same HLSL with `[unroll]`: 360 bytes against 228.
SGL writes out its own `@unroll` copies, so each copy declares its arrays anew, and DXC does not merge allocas whose lifetimes do not overlap.
Measured with the pinned DXC 1.9.2602.24 at `-T cs_6_6 -O3`, as DXIL; whether a driver overlaps them on the GPU is not measured.

Two ways out, neither local:

- declare a copied loop's arrays once, ahead of the first copy, when no copy reads what an earlier one left;
- or leave a loop SGL can prove the target unrolls as the target's own `[unroll]`.

Found while reproducing a port's BC7 slowdown, whose 2.4 times came from an integer `const` table filled per invocation, fixed in EMIT-230's `is_constant_leaf`.
