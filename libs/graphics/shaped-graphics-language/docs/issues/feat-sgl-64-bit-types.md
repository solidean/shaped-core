---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# 64-bit integers and doubles

SGL has no `int64`, `uint64` or `double`.
dx12 and vulkan have all three behind device features, metal has 64-bit integers and no doubles, and WebGPU has neither.

So each would be a feature, refused where a target lacks it, like `half` (CHK-381).
64-bit integers are the more useful pair: hashing, packed keys, and 64-bit atomics ([feat-sgl-float-and-64-bit-atomics](feat-sgl-float-and-64-bit-atomics.md)).
