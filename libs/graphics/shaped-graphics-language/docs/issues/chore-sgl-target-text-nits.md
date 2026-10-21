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

- HLSL and MSL compare `int(q.CommittedStatus()) == 0` rather than `COMMITTED_NOTHING` and `intersection_type::none`.
  The prelude's `ray_query_committed_kind` is an `int`, whose 0, 1 and 2 are both targets' own values.
  Naming them needs a `@builtin` enum whose cases each target spells, which no builtin enum does yet: EMIT-108 writes a case as its value.
- HLSL builds a struct literal as `T v; v.a = ...;`, which loses the `const`, where WGSL writes a constructor: `ray r`, `dispatch_args_value`.
  HLSL's one alternative is a declaration's `T v = { a, b };`, which is positional, and EMIT-55 builds member by member to keep the field names.
  So this is the trade EMIT-55 decided, to be reopened there rather than here.
- WGSL converts `vec2f(vec2i(textureDimensions(t, 0)))` twice.
  The inner conversion is the size builtin's and the outer the program's `as`, so folding them couples the conversion's writer to what it converts.

Done: HLSL's doubled parentheses around a candidate's type test, and WGSL's `x >> u32(12)` with MSL's mask on a literal count.
