---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/hlsl.cc
---

# HLSL writes `RayQuery<RAY_FLAG_NONE>` where a trace's flags are constants

`trace_cells_opaque` writes `RayQuery<RAY_FLAG_NONE> q; q.TraceRayInline(scene_world, uint(1 | 512), ...)`.
DXR lets a driver specialize on the template's flags, `SKIP_PROCEDURAL_PRIMITIVES` and a forced opaque among them.
Under `force_opaque` the whole `while (q.Proceed())` candidate body is still written: twelve matrix fetches and a decision that is constant.
A fix puts the constant part of the flags into the template, and leaves out the candidate loop where the flags make every candidate opaque.
