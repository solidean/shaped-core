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
