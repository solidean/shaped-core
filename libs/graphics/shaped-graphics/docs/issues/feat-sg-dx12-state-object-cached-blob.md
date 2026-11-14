---
created: 2026-10-17
parent: epic-sg-raytracing-pipeline-gaps
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_raytracing_pipeline.cc
  - symbol: sg::backend::dx12::dx12_raytracing_pipeline::cached_pipeline_data
---

# A cached blob for a dx12 raytracing state object

`dx12_raytracing_pipeline::cached_pipeline_data()` returns empty.
`ID3D12StateObjectProperties` has no `GetCachedBlob` equivalent, so serializing one would need collection state objects or add-to-state-object.
Metal has the same gap for a different reason: [question-sg-metal-pipeline-blob](question-sg-metal-pipeline-blob.md).

Vulkan's ray-tracing pipelines serialize their VkPipelineCache, and the persistent tier stores it as for raster (2026-11-14); dx12 stores an empty placeholder.
The one D3D12 candidate found is the State Object Database (`ID3D12StateObjectDatabase`, Agility SDK 1.618, with enumeration and transactions in 1.620).
It serializes a ray-tracing state object as its full description with the DXIL, as the input to precompiled shader delivery, and its spec warns that recording is slow under multithreaded creation.
So it stores descriptions rather than driver output: what it would save a run is unmeasured, and a prototype behind the Agility SDK is the next step.
Meanwhile the driver's own disk cache decides: on an RTX 4090, `path_trace`'s state objects took 1.1–2.8 s each under the build server's load, and well under a second on a warm, quiet run.
