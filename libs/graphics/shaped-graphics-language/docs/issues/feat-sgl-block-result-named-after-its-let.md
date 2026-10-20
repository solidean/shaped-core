---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_exprs.cc
---

# A block's result is a var of its own, which the let that takes it then copies

The cube writes `float3 shade_result; ... const float3 lit = shade_result;`, and bc7 `int if_4_result; ... const int pmode = if_4_result;`.
Results are handed on through chains, `step_1_result` to `if_result` to `c`.
Where a block expression is the whole value of a `let`, E1 could name the result var after the let and drop the copy, and an inner result could assign the outer one directly.
A two-armed `if` over cheap operands without an effect could be a `select` or `?:` instead.
