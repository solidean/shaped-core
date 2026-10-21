---
created: 2026-10-17
parent: epic-sg-acceleration-structure-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/raytracing/acceleration_structure.hh
  - symbol: sg::accel_build_flag::allow_update
---

# Refit / update of an acceleration structure

A refit reuses the topology.
It needs `allow_update` at build, plus `PERFORM_UPDATE` and the source structure at update time.
The flag and `update_scratch_size_in_bytes()` exist; the update call does not.
Metal's native refit is in the [epic](epic-sg-acceleration-structure-gaps.md).
