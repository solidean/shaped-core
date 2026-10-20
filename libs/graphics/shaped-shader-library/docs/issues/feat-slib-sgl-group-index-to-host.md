---
created: 2026-10-20
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/pipeline.hh
---

# A pipeline's SGL group numbers reach the host

A pipeline's SGL groups are numbered by position, and the host is never told the number.
A generated binding struct has no `group_index`, so `scope.bind<G>(group)` does not compile for it, and a caller writes `bind_group(1, ...)` from the order it worked out by hand.
Reordering two declarations then binds the wrong group without any diagnostic.
How to expose it is open: a per-pipeline index table in the generated pipeline type is one shape, a `group_index` fixed by the binding declaration another.
