---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_compute_pipeline.hh
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_raster_pipeline.hh
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_raytracing_pipeline.hh
  - test: sg metal - a compute pipeline builds from a metal library
  - symbol: sg::compute_pipeline::cached_pipeline_data
---

# The metal backend serializes no pipeline blob

`compute_pipeline::cached_pipeline_data()` returns empty there and `used_cached_pipeline()` is always false.
So a caller persisting a blob across runs gets nothing to persist, and every build is a cold one.

It is not a missing call: sg's surface is **one blob per pipeline**, and Metal 4's `MTL4Archive` is **one store per compiler** that accumulates every pipeline built through it.
dx12 hands back a `ID3D12PipelineState` blob and vulkan gives each pipeline a `VkPipelineCache` of its own; neither shape exists here.

The options:

- An archive per context, serialized as a whole — which sg's per-pipeline key cannot address.
- An archive per pipeline — one compiler each, which is heavy.
- A surface change so a backend may own the store.

Pinned as deliberate by `sg metal - a compute pipeline builds from a metal library`, so closing it is a failing test rather than something nobody notices.
