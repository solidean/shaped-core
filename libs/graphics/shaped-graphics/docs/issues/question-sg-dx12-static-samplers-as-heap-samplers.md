---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_pipeline_layout.hh:79
  - issue: perf-sg-raster-pipelines-never-persist
---

# Should dx12 lower static samplers to heap samplers, so pipelines with them can persist?

`dx12_pipeline_layout::has_static_samplers` drops the cached blob of every dx12 pipeline whose root signature has a static sampler, for NVIDIA's restore bug (docs/bugs-external/d3d12-cached-pso-static-sampler-mixup).
It reproduced on an RTX 4090 with driver 32.0.16.1088 (2026-11-14), so it is not one card's or one driver's.
Every such pipeline compiles fresh in every run, which is where shaped-viewer's slowest creates are (`light_thin`, 9.4 s).

The options:
- (a) Lower static samplers to sampler descriptors in a heap table on dx12: no root signature carries one, so the bug's precondition is gone. A binding change in the dx12 backend, bounded by the 2048-sampler heap.
- (b) Record which static-sampler variants each shader has in the store, and skip blobs only for shaders with more than one. Narrower, but subtle across processes.
- (c) Keep it, and rely on the driver's own disk cache.
