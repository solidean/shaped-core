---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# What a compute entry point still cannot lower

Texel atomics, and the `affine3`, `packed` and `bool32` types.
A float atomic with no device add, a body in target text, a sample's offset, and prints and asserts through the debug window.
The workgroup prologue of a robustness profile.
Ray queries are their own child.

DXIL lowers 93 of the 112 compute entry points the fixtures hold, and adds these of its own:
a 64-bit atomic, a `double`'s maths, which the text route writes as helpers of HLSL (EMIT-219), the subgroup shuffles and an inclusive fold, a texture's sample count, `determinant` and the packs.
Each is a named gap, and a row of a table where DXIL has the operation.
