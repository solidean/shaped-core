---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# What a compute entry point still cannot lower

Binding arrays, texel atomics, a gather and a compare sample, and the `affine3` and `bool32` types.
A float atomic with no device add, a body in target text, and prints and asserts through the debug window.
The workgroup prologue of a robustness profile.
Ray queries are their own child.
