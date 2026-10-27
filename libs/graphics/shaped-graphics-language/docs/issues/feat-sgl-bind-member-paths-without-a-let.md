---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
  - symbol: bind_parameter
---

# An inlined call binds a member of an immutable local to a let of its own

Across all dx12 text of the sampled shaders, 284 of 2442 `const` lets are `name = name;` and 332 are `name = name.member;`: `const float x = v.x;`, `const int i = id.x;`, `const float4 c0_1 = c0;`.
`bind_parameter` substitutes only a bare immutable local or a literal.
A member or swizzle path of an immutable local could be substituted the same way, and a `var` the inlined body cannot see written as well.
svgf's `atrous_cs` binds `n_p_N = nd_p.xyz` 25 times, which L1 cannot merge since each first copy stands inside an earlier sibling `if`.

Tried on 2026-10-20 and taken back: a substituted `v.x` nests one level deeper at every use than the local it replaced.
shaped-viewer's `openpbr_microfacet.sgl` then passes CHK-268's 40 levels in `ggx_lambda`, even with paths of one step.
So this waits for the iterative walks `legalize/impl/walk.hh` names as the fix for that limit, or for a rule that weighs a use's depth.
