---
created: 2026-11-14
priority: next
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/pipeline_cache.cc:516
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_pipeline_layout.hh:79
  - symbol: sg::pipeline_cache::acquire_raster_pipeline
  - test: shaped-viewer-test
  - issue: feat-sg-dx12-state-object-cached-blob
---

# Raster pipelines never reach the persistent PSO tier, so a slow driver compile is paid every run

`acquire_raster_pipeline` is in-memory only: its key holds the pipeline layout's address, so it means nothing in the next process, and no raster PSO blob is ever stored or offered.
`caches.md` states this as deliberate, with the layout's structural hash as the step that would lift it.

Measured from shaped-viewer-test's run recordings (2026-11-14, RTX 4090, driver 32.0.16.1088):
- Four raster creates of 8.5–10 s each, plus one compute create of 7.4 s, and nothing else in the binary moves while they run: about 32 s of a 106 s run.
- A second run had the same stalls at 12–13 s each, so nothing carried over.
- The other 94 raster creates average 1.5 ms.
- The waiting test logs "size forward: first samples without stand-ins 8.9 s and 3056 idle frames".

Two further reasons a blob would be refused even with a persistent tier:
- dx12 drops the blob for any layout with static samplers, compute and raster alike (`dx12_pipeline_layout::has_static_samplers`, a driver bug seen on an RTX 5070 Ti).
  Hypothesis: the 7.4 s compute stall is such a pipeline; the next recording says, since creates now record `sg.pipeline.cached_blob_offered` and `_used`.
- Raytracing pipelines have no persistent tier either (feat-sg-dx12-state-object-cached-blob).

The work: key raster pipelines by the layout's structural hash, route them through the same bcache acquire compute uses, and retest the static-sampler workaround on the current driver.
