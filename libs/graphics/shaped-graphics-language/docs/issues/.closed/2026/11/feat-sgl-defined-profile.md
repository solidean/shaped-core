---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/legalization.md
closed: 2026-11-05
resolution: fixed
---

# A profile that gives every operation with no value a value

WGSL defines integer division by zero and clamps every out-of-bounds access, polyfilled on every target.
A `defined` profile would do the same for a shader that must survive untrusted input, at the cost of every guarded operation.
Memory access can lean on what the targets do already: WebGPU's robustness, vulkan's `robustBufferAccess2`, dx12's zero for an out-of-range descriptor read.
The integer operations need the polyfill.

## Done on 2026-11-05

The profile `define` (LEGAL-68), `SC_SHADER_UNDEFINED=DEFINE`: every guard of LEGAL-65 and LEGAL-66 replaces its operand with the safe value and goes on.
Integer division matches WGSL's values exactly; an array, buffer or `bytes` index falls to 0, which WGSL allows as one of its outcomes.
Left: a buffer or a `bytes` that holds nothing, where index 0 is still past the end, and workgroup memory read before any write.
