---
created: 2026-11-25
tags: [amd, raytracing, external-tool]
refs:
  - file: libs/graphics/shader-analysis
  - issue: feat-sgl-disassembly-vendor-isa
  - issue: feat-sgl-shader-machine-simulation
---

# Driver-compiled register pressure and spills via AMD's Radeon GPU Analyzer

shan compares two modules of one entry point, SGL's against DXC's above all, and deliberately models no GPU.
AMD's Radeon GPU Analyzer (RGA) compiles the same input with AMD's own driver compiler, offline, for a chosen target, and reports VGPR and SGPR counts, spills and the ISA.
That answers what shan cannot: whether a difference between SGL's output and DXC's is still there once the driver has compiled it.

The case that motivates it is ray tracing.
RGA compiles a whole DXR state object, so each shader's VGPR and spill figures come from the pipeline as the driver builds it, not from a library on its own.
The first target is the RX 9060 XT (RDNA4).

How this differs from its neighbors:
`feat-sgl-disassembly-vendor-isa` shows the ISA an SGL entry point becomes, through every vendor's tools.
This one is a driver-side check on shan's diff, AMD alone, with the ray-tracing pipeline as the unit.
The two likely share the RGA invocation, and whichever lands first owns it.

Known so far, none of it verified yet:
- RGA's DX12 mode, which is what compiles DXR state objects, is believed to be Windows-only; Linux has the Vulkan / SPIR-V mode.
  Windows-only is acceptable.
- Its figures hold for AMD's compiler alone and are no cost model for any other vendor.

Open:
- Fetched on request, like `extern/agility`, or found on `PATH`; and its license.
- A shan back end, a `dev.py` command, or an option of `shan diff`, keeping shan's own dependencies to clean-core and babel-data.
