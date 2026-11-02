---
created: 2026-10-18
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/rendering-roadmap.md
---

# The ray-query polyfill exists on webgpu alone

sv's roadmap lights its hybrid mode by hardware ray tracing or, where a device has none, by the polyfill ([rendering-roadmap.md](../../../shaped-viewer/docs/plans/rendering-roadmap.md)).
A dx12 or vulkan device without ray tracing has no polyfill, so the hybrid there falls back to raster's direct lighting until sg brings it to those backends.
