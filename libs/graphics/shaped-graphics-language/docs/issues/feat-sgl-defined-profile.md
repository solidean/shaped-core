---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/legalization.md
---

# A profile that gives every operation with no value a value

WGSL defines integer division by zero and clamps every out-of-bounds access, polyfilled on every target.
A `defined` profile would do the same for a shader that must survive untrusted input, at the cost of every guarded operation.
Memory access can lean on what the targets do already: WebGPU's robustness, vulkan's `robustBufferAccess2`, dx12's zero for an out-of-range descriptor read.
The integer operations need the polyfill.
