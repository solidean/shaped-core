---
created: 2026-11-14
refs:
  - symbol: sg::backend::dx12::dx12_raster_pipeline::skips_cached_blob
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_raster_pipeline.cc
  - test: "sg - an SGL mesh stage of lines draws lines, which its pipeline rasterizes as lines"
---

# A dx12 mesh-shaded pipeline's cached blob is refused on every restore

Once raster pipelines reached the persistent tier (2026-11-14), every dx12 mesh-shaded pipeline's restore was refused, on dx12-hw and dx12-native-dxil alike.
The debug layer reports "a cached PSO which does not match the rest of the description", which fails the test by the log rule.
It is deterministic: the run after a refusal stores a fresh blob, and the run after that refuses it again.
Plain and view-instanced raster pipelines restore without it, 134 creates a shaped-viewer run (RTX 4090, driver 32.0.16.1088).

`create_mesh_shaded` builds the same stream for the store and the restore, so the difference is not visible in our code.
Not yet known whether the runtime, the debug layer or the driver refuses, nor whether a blob stored and restored in one process is refused too.

Until then a mesh-shaded pipeline neither restores nor hands out a blob (`dx12_raster_pipeline::skips_cached_blob`).
