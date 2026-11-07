---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# What a compute entry point still cannot lower

Texel atomics, and the `packed` and `bool32` types, and an `affine3` in GPU memory.
A float atomic with no device add, a body in target text, and a sample's offset.
The workgroup prologue of a robustness profile (EMIT-229), which keeps five DXIL entry points from each profile: `sgl_written` and the zeroing are no part of `plan`, and the text writes them by hand.

DXIL lowers 105 of the 115 compute entry points the fixtures hold, and adds these of its own:
a 64-bit atomic, a `double`'s maths, which the text route writes as helpers of HLSL (EMIT-219), the subgroup shuffles and an inclusive fold, a texture's sample count, `determinant` and the packs.
Each is a named gap, and a row of a table where DXIL has the operation.
