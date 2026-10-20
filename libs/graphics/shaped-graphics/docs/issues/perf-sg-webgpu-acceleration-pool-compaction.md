---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_acceleration_pool.cc
---

# Compacting the webgpu acceleration pool

A freed region is reused first-fit, and the pool only grows, so a scene that rebuilds often fragments it.
