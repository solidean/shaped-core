---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/atomics.md
---

# Float atomics and 64-bit atomics

Only 32-bit `int` and `uint` atomics exist; a float or a 64-bit one is refused statically (CHK-296).
[incubator/atomics.md](../spec/incubator/atomics.md) has the ideas.

Float add is what splatting, histograms and gradient accumulation want, autodiff's reverse mode included ([epic-sgl-autodiff](epic-sgl-autodiff.md)).
Support is uneven: dx12 and vulkan have float add behind features, metal has it on buffers, and WebGPU has none.
So each is a feature, and a compare-and-swap loop is a candidate polyfill where a target has 32-bit atomics alone.
64-bit atomics wait on [feat-sgl-64-bit-types](feat-sgl-64-bit-types.md).
