---
created: 2026-10-17
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
---

# Quads of a two-dimensional compute workgroup

CHK-380 holds a compute stage's quad operations to a workgroup of one row, the one shape whose quads every target forms alike.
HLSL forms 2x2 squares of the threads' ids in any other, which WGSL and MSL can only reach by shuffling with lanes computed from `@local_thread_id`.
It lands once a shader wants square quads, written as such a shuffle wherever the target's own quads are lanes.
