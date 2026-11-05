---
created: 2026-11-05
parent: epic-sgl-shader-disassembly
---

# Show the GPU ISA an entry point becomes, through each vendor's tools

The intermediate form says what the driver was asked for; the ISA says what it did, which is where a driver's own simplification shows.
AMD's Radeon GPU Analyzer compiles DXIL and SPIR-V offline to RDNA ISA with register counts; Intel, NVIDIA and Apple each have a dump or a tool of their own.
What each vendor offers offline, without the device, decides how much of this runs in CI and how much on the machine that has the GPU.
