---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_pipeline_layout.hh:79
  - issue: perf-sg-raster-pipelines-never-persist
closed: 2026-11-14
resolution: fixed
---

# Should dx12 lower static samplers to interned heap samplers, so pipelines with them can persist?

`dx12_pipeline_layout::has_static_samplers` drops the cached blob of every dx12 pipeline whose root signature has a static sampler.
It works around NVIDIA's restore bug, docs/bugs-external/d3d12-cached-pso-static-sampler-mixup.
It reproduced on an RTX 4090 with driver 32.0.16.1088 (2026-11-14), so it is not one card's or one driver's.
Every such pipeline compiles fresh in every run, which is where shaped-viewer's slowest creates are (`light_thin`, 9.4 s).

## The proposal

Each sampler state the backend meets is interned once into a fixed region of the shader-visible sampler heap, and its slot is never freed.
A static sampler becomes a root parameter of its own: a descriptor table of one sampler at the shader's register.
Binding the pipeline points each such table at its state's interned slot, so nothing is allocated per draw and nothing has a lifetime.

The bug needs two root signatures that differ only in a static sampler.
Here a clamping and a repeating variant of one shader have the same root signature, so they are one PSO, and its blob is safe to restore.
The shader is untouched: `register(s0)` reads the same whether a static sampler or a table fills it, so DXC's HLSL works as SGL's does.

What it costs, to be measured:
- One root DWORD per static sampler, which costs none today; the 64-DWORD budget bounds how many a layout may have.
- One `SetGraphicsRootDescriptorTable` or its compute twin per static sampler at each pipeline bind.
- Interned slots out of the 2048-sampler heap, shared with the binding groups' own samplers; a run meets a few dozen states.

## Alternatives weighed

- A fixed set of the most common samplers as static samplers in every root signature.
  The root signatures then agree, but a shader must name the register of the state it wants, so it only works where we choose the registers: SGL's own output, not DXC's HLSL.
- Recording per shader in the store which sampler variants exist, and skipping blobs only for shaders with more than one: narrower, but subtle across processes.
- Keeping the workaround and relying on the driver's own disk cache, which the build server's load defeats.
