---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/raytracing-pipeline.md
---

# Raytracing pipeline: what is still open

The trace path is in on dx12, vulkan and metal — see [concepts/raytracing-pipeline.md](../concepts/raytracing-pipeline.md).

webgpu has no pipeline, deliberately.
A megakernel over the ray-query polyfill is the shape one would take, and a path tracer on ray queries is the better route there for now.
Its BVH quality is [perf-sg-webgpu-bvh-spatial-sort](perf-sg-webgpu-bvh-spatial-sort.md).
