---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/keyed_cache.hh
---

# Nothing evicts a parameter block

The LRU pool went away with the pin it existed to hold, so the set is bounded by the distinct (material, mesh) pairs a process ever draws rather than by a budget.
Each is one small buffer plus its slots; a long-lived session cycling through materials would grow without bound.
Folding the records onto `impl::keyed_cache` alongside the other managers is what would bound it.
