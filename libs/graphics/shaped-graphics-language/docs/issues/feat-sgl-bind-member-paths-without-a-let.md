---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
  - symbol: bind_parameter
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-20T15:24Z
---

# An inlined call binds a member of an immutable local to a let of its own

Across all dx12 text of the sampled shaders, 284 of 2442 `const` lets are `name = name;` and 332 are `name = name.member;`: `const float x = v.x;`, `const int i = id.x;`, `const float4 c0_1 = c0;`.
`bind_parameter` substitutes only a bare immutable local or a literal.
A member or swizzle path of an immutable local could be substituted the same way, and a `var` the inlined body cannot see written as well.
svgf's `atrous_cs` binds `n_p_N = nd_p.xyz` 25 times, which L1 cannot merge since each first copy stands inside an earlier sibling `if`.
