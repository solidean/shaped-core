---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_erase.cc
closed: 2026-11-05
resolution: fixed
---

# A lowered assert before a barrier ends its invocation there

With diagnostics, LEGAL-64 lowers a failed `assert` to its print and the end of the invocation, at the assert.
In a compute stage with a `workgroup_barrier` behind it, the barrier is then reached by only part of the workgroup, which is undefined; Tint's uniformity analysis may also refuse the text.
LEGAL-65's guards already wait for the last such statement through a flag; the assert lowering should share that mechanism.
