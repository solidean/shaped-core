---
created: 2026-10-22
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_lets.cc
---

# A struct a function returns from two branches is built whole, all of whose fields MSL fetches one element at a time

LEGAL-61 splits a struct read only field by field where one `let` or one assignment built it.
`committed_of` returns a `triangle_hit` from two branches, a miss and a hit, so its result `var` is assigned twice and stays whole.
MSL then writes all 24 `get_committed_*_transform()[i][j]` reads of the hit's six rows, one element at a time, even where the caller reads `t` alone.
Metal's compiler likely drops the fields nothing reads, but whether it merges the intrinsic calls is not known: no Mac ran it.
Splitting a `var` assigned a construction in each branch of one `if`, each field a `var` of its own, would cover it.
