---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
closed: 2026-11-06
resolution: fixed
---

# A DXIL writer over the native layer

LLVM 3.7 bitcode and the DXIL container, with the retail bypass digest the epic's spike found D3D12 takes.
After SSA construction.

## Done

`ir/dxil/` writes a compute entry point as a container on shader model 6.6, and sg's route loads it under the debug layer.
The epic has the design and the numbers.
What is left is graphics stages, ray tracing and the gaps, each its own child.
Abbreviations in the bitcode would close the size gap to DXC's DXIL part.
