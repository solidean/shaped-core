---
created: 2026-10-30
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/builtins.sgl
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_vector_math.cc
---

# Scalar-broadcast overloads written once, generically, instead of per type

`min`, `max` and `clamp` get overloads whose bound is the element's scalar, `max(v, 0.0)` on a `float3`, one per vector type and width.
That is a family loop in the registry, and it makes the generated prelude, already about 3900 lines, longer still by the same pattern.

- **Wanted**: one generic declaration per function, over a vector family and its element type, once SGL's generics can say "a vector of `T`, and a `T`".
- **Then** the per-type overloads go, from the registry and from `builtins.sgl` alike, and the broadcast list can grow (`step`, `smoothstep`) at no cost in prelude size.
- **Blocked on** generics over the prelude's vector families; until then the loop is the stand-in.
