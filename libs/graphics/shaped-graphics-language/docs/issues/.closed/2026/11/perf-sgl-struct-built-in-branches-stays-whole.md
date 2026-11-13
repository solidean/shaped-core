---
created: 2026-10-22
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_lets.cc
closed: 2026-11-09
resolution: wontfix
---

# A struct a function returns from two branches is built whole, all of whose fields MSL fetches one element at a time

LEGAL-61 splits a struct read only field by field where one `let` or one assignment built it.
`committed_of` returns a `triangle_hit` from two branches, a miss and a hit, so its result `var` is assigned twice and stays whole.
MSL then writes all 24 `get_committed_*_transform()[i][j]` reads of the hit's six rows, one element at a time, even where the caller reads `t` alone.
Metal's compiler likely drops the fields nothing reads, but whether it merges the intrinsic calls is not known: no Mac ran it.
Splitting a `var` assigned a construction in each branch of one `if`, each field a `var` of its own, would cover it.

## Resolution

Measured on 2026-11-09 with Apple's Metal toolchain 27.1: Metal's compiler does both, so splitting the `var` would buy nothing on metal.
A compute entry tracing inline and reading `t` alone emits all 24 element reads, and its AIR at `-O1`, `-O2` and `-Os` holds no transform intrinsic at all.
One that reads `object_to_world` too keeps exactly one call, `air.get_committed_object_to_world_transform…`, for its 12 element reads, and none for the matrix it leaves unread.
ssc::msl compiles at `-O2` by default, and the runtime source path optimizes as well.
