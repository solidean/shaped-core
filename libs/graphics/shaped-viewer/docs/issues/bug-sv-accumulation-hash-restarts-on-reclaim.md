---
created: 2026-10-17
refs:
  - symbol: trace_hash
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
---

# The accumulation hash can restart a converged image for something that is not a scene change

`trace_hash` hashes each record's `vertices` and `indices` bindless indices plus its BLAS pointer.
Those are stable while a working set is.
They move the moment something reclaims — a bindless array that fills and rotates elements, or a mesh evicted from `mesh_manager` and re-uploaded under a new BLAS handle.
Same scene, restarted accumulation, nothing reporting it.

The rule the fix has to keep is that the hash may be imprecise in **one direction only**.
A render setting that feeds the hash without changing the image — a "high quality mirror" toggle in a scene with no mirrors — costs a restart nobody minds.
A restart on a static scene the user did nothing to is not acceptable.
Precise content hashing for accumulation is what eventually fixes it.
