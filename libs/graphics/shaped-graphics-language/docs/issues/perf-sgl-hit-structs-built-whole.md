---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# A hit or a candidate is built field by field where one field is read

HLSL's `trace_cells` writes 30 lines of `c.* = ...` and six `Candidate*3x4()` calls, where `cutout` reads only `c.primitive_index`.
MSL calls `get_candidate_object_to_world_transform()` 24 times per candidate, once per element.
Replacing an aggregate whose every use is a field read by its fields would leave the reads alone, and MSL could bind the transform once.
