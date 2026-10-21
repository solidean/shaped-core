---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/hlsl.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/wgsl.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-21T18:52Z
---

# Small things the target text could spell better

Each is small enough to go with whatever next touches the writer.

- HLSL `if ((q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE))` doubles its parentheses.
- HLSL and MSL compare `int(q.CommittedStatus()) == 0` rather than `COMMITTED_NOTHING` and `intersection_type::none`.
- HLSL builds a struct literal as `T v; v.a = ...;`, which loses the `const`, where WGSL writes a constructor: `ray r`, `dispatch_args_value`.
- WGSL writes `x >> u32(12)` for `x >> 12u`.
- WGSL converts `vec2f(vec2i(textureDimensions(t, 0)))` twice.
