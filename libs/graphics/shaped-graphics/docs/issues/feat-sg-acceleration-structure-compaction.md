---
created: 2026-10-17
parent: epic-sg-acceleration-structure-gaps
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_raytracing.cc
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_raytracing.cc
  - symbol: sg::accel_build_flag::allow_compaction
---

# Acceleration structure compaction

Build a BLAS with `allow_compaction`, query the compacted size, and copy into a smaller buffer.
dx12 and vulkan translate the flag, and neither implements the query and copy, so it is the one build-time flag that does nothing yet.
Metal's native compaction is in the [epic](epic-sg-acceleration-structure-gaps.md).
