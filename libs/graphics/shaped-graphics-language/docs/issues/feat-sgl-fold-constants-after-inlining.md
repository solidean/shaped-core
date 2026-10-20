---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_lets.cc
  - issue: feat-sgl-literal-folding
---

# Nothing is folded once inlining and unrolling have made it constant

The emitted text holds `sg_acceleration_roots[0 / 4][0 % 4]`, `if ((0 & 64) == 64) && is_opaque_1`, `uint(0 | 512)`, `kernel[-2 + 2]` and `p + vec2i(0, 0)`.
bc7 writes `if (((0u >> uint(i_3)) & 1u) != 0u) continue;` inside an unrolled loop, and the trace `const int d = hit_decision_accept; if (d != hit_decision_ignore)`.
`feat-sgl-literal-folding` folds what the source writes; this is what substitution makes constant, an argument that was a literal among it.
A pass over the core tree beside L1 and L2 could fold integer and boolean operators over constants, and a branch on a constant the flattener did not see as one.
Folding floats there is the open half: a target rounds each step, so a fold has to round as the target does.
