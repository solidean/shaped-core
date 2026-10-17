---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.hh
  - symbol: sv::gpu_resource_manager
---

# The bindless lock is global where the hazard is per-recording

A routine that legitimately wants to acquire mid-recording — a stream just landed, a later trace in the same list wants it — has one correct answer.
That answer is a fresh snapshot, rebound for the dispatches after it.
A clean `snapshot()` is the cached handle, so it is nearly free.
The lock refuses instead.
